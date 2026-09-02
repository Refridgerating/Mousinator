#!/usr/bin/env python3
"""Concurrency and lifecycle tests for the optional vision worker."""

from __future__ import annotations

import threading
import time
import unittest
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from host.vision import Detection, VisionService


TARGET = Detection(0, "animal_mouse", 0.9, 20, 20, 40, 40)


class FakeCamera:
    def __init__(self):
        self.calls = 0
        self.frame = np.zeros((100, 100, 3), dtype=np.uint8)

    def get_frame(self, *, copy=True):
        self.calls += 1
        return self.frame.copy() if copy else self.frame


class FakeDetector:
    class_names = {0: "animal_mouse"}

    def __init__(self, detections=(TARGET,), *, delay=0.0, failure=None):
        self.detections = detections
        self.delay = delay
        self.failure = failure
        self.calls = 0
        self.active = 0
        self.maximum_active = 0

    def validate_target_class(self, target_class):
        if target_class not in self.class_names.values():
            raise ValueError("target class is absent")

    def detect(self, frame):
        del frame
        self.calls += 1
        self.active += 1
        self.maximum_active = max(self.maximum_active, self.active)
        try:
            if self.delay:
                time.sleep(self.delay)
            if self.failure is not None:
                raise self.failure
            return self.detections
        finally:
            self.active -= 1


class BlockingDetector(FakeDetector):
    def __init__(self):
        super().__init__()
        self.entered = threading.Event()
        self.release = threading.Event()

    def detect(self, frame):
        del frame
        self.calls += 1
        self.entered.set()
        self.release.wait(2.0)
        return self.detections


def wait_for(predicate, timeout=2.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.005)
    raise AssertionError("condition was not reached before timeout")


def create_service(camera, factory, *, detection_fps=20.0):
    return VisionService(
        camera,
        model_path=Path("fake.pt"),
        target_class="animal_mouse",
        confidence=0.5,
        detection_fps=detection_fps,
        inference_size=640,
        dead_zone_x=0.05,
        dead_zone_y=0.05,
        detector_factory=factory,
    )


class VisionServiceTests(unittest.TestCase):
    def test_disabled_default_idempotence_load_once_rate_cap_and_performance(self):
        camera = FakeCamera()
        detector = FakeDetector()
        factory_calls = []

        def factory(path, confidence, inference_size):
            factory_calls.append((path, confidence, inference_size))
            return detector

        service = create_service(camera, factory, detection_fps=20.0)
        self.addCleanup(service.close)
        service.start()
        time.sleep(0.08)
        self.assertEqual(camera.calls, 0)
        self.assertFalse(service.status()["vision_enabled"])

        self.assertTrue(service.enable())
        self.assertFalse(service.enable())
        wait_for(lambda: detector.calls >= 3)
        elapsed_calls = detector.calls
        time.sleep(0.12)
        self.assertLessEqual(detector.calls - elapsed_calls, 3)
        self.assertEqual(detector.maximum_active, 1)
        status = service.status()
        self.assertTrue(status["vision_ready"])
        self.assertIsNotNone(status["inference_ms"])
        self.assertGreater(status["detection_fps"], 0)
        self.assertEqual(status["vision_frame_width"], 100)
        self.assertEqual(status["vision_frame_height"], 100)

        self.assertTrue(service.disable())
        self.assertFalse(service.disable())
        calls_after_disable = detector.calls
        time.sleep(0.09)
        self.assertEqual(detector.calls, calls_after_disable)
        self.assertTrue(service.enable())
        wait_for(lambda: detector.calls > calls_after_disable)
        self.assertEqual(len(factory_calls), 1)

    def test_disable_rejects_an_in_flight_result(self):
        camera = FakeCamera()
        detector = BlockingDetector()
        service = create_service(camera, lambda *_args: detector)
        self.addCleanup(service.close)
        service.start()
        service.enable()
        self.assertTrue(detector.entered.wait(1.0))
        self.assertTrue(service.disable())
        detector.release.set()
        time.sleep(0.05)
        status = service.status()
        self.assertFalse(status["vision_enabled"])
        self.assertEqual(status["tracking_state"], "SEARCHING")
        self.assertEqual(status["detections"], [])
        self.assertIsNone(status["target_bbox"])

    def test_slow_inference_has_no_concurrent_or_accumulating_work(self):
        camera = FakeCamera()
        detector = FakeDetector(delay=0.08)
        service = create_service(camera, lambda *_args: detector, detection_fps=50.0)
        self.addCleanup(service.close)
        service.start()
        service.enable()
        time.sleep(0.27)
        service.disable()
        self.assertLessEqual(detector.calls, 4)
        self.assertEqual(detector.maximum_active, 1)
        self.assertEqual(camera.calls, detector.calls)

    def test_inference_failure_disables_only_vision_and_shutdown_joins(self):
        camera = FakeCamera()
        detector = FakeDetector(failure=RuntimeError("synthetic inference failure"))
        service = create_service(camera, lambda *_args: detector)
        service.start()
        service.enable()
        wait_for(lambda: not service.status()["vision_enabled"])
        status = service.status()
        self.assertIn("synthetic inference failure", status["vision_error"])
        self.assertEqual(status["tracking_state"], "SEARCHING")
        self.assertEqual(camera.get_frame(copy=False).shape, (100, 100, 3))
        service.close()
        service.close()


if __name__ == "__main__":
    unittest.main()
