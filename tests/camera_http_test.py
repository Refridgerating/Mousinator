#!/usr/bin/env python3
"""Flask API tests for the camera browser service."""

from __future__ import annotations

import tempfile
import unittest
import sys
from collections import namedtuple
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from host.camera.camera_service import CameraService, CapturedFrame
from host.camera.storage import MediaStorage
from host.camera.stream_server import create_app
from host.vision import VisionService
from host.vision.detector import DetectorConfigurationError


DiskUsage = namedtuple("DiskUsage", "total used free")


class FakeCameraBackend:
    native_resolution = (8, 6)

    def __init__(self):
        self.recording = False

    def start(self):
        pass

    def stop(self):
        pass

    def get_frame(self):
        return CapturedFrame(np.zeros((4, 6, 3), dtype=np.uint8), {})

    def capture_sensor_native(self):
        return CapturedFrame(np.zeros((6, 8, 3), dtype=np.uint8), {})

    def start_recording(self, path):
        del path
        self.recording = True

    def stop_recording(self):
        self.recording = False

    def wait_for_jpeg(self, after_sequence, timeout_s):
        del timeout_s
        return after_sequence + 1, b"\xff\xd8fake-jpeg\xff\xd9"


class FakeDetector:
    class_names = {0: "animal_mouse"}

    def validate_target_class(self, target_class):
        if target_class != "animal_mouse":
            raise DetectorConfigurationError("target class is absent")

    def detect(self, _frame):
        return ()


class AbsentClassDetector(FakeDetector):
    def validate_target_class(self, _target_class):
        raise DetectorConfigurationError("target class is absent")


class CameraHttpTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.free_bytes = 4 * 1024 * 1024 * 1024
        storage = MediaStorage(
            Path(self.temporary_directory.name) / "data",
            1024 * 1024 * 1024,
            disk_usage=lambda _path: DiskUsage(
                8 * 1024 * 1024 * 1024,
                4 * 1024 * 1024 * 1024,
                self.free_bytes,
            ),
        )
        self.service = CameraService(
            FakeCameraBackend(),
            storage,
            width=1280,
            height=720,
            fps=30,
            capture_interval_s=2,
        )
        model_path = Path(self.temporary_directory.name) / "model.pt"
        model_path.touch()
        self.vision = self._new_vision(
            model_path,
            detector_factory=lambda *_args: FakeDetector(),
        )
        self.vision.start()
        self.app = create_app(self.service, self.vision)
        self.client = self.app.test_client()
        self.addCleanup(self._close_service)

    def _new_vision(self, model_path, detector_factory=None):
        return VisionService(
            self.service,
            model_path=model_path,
            target_class="animal_mouse",
            confidence=0.5,
            detection_fps=5,
            inference_size=640,
            dead_zone_x=0.05,
            dead_zone_y=0.05,
            detector_factory=detector_factory,
        )

    def _close_service(self):
        self.vision.close()
        try:
            self.service.close()
        except RuntimeError:
            pass

    def test_page_status_and_mutating_endpoints(self):
        self.service.start()
        page = self.client.get("/")
        self.assertEqual(page.status_code, 200)
        self.assertIn(b"Sentry Camera", page.data)
        self.assertIn(b'<canvas id="overlay"', page.data)
        self.assertIn(b"/api/vision/start", page.data)

        status = self.client.get("/api/status").get_json()
        self.assertTrue(status["camera_running"])
        self.assertEqual(status["resolution"], {"width": 1280, "height": 720})
        self.assertEqual(status["fps"], 30)
        required_vision_fields = {
            "vision_enabled", "vision_ready", "vision_model", "target_class",
            "confidence_threshold", "inference_size", "detection_fps_limit",
            "dead_zone_x", "dead_zone_y", "vision_error", "tracking_state",
            "target_detected", "target_confidence", "target_bbox", "target_center",
            "pan_error", "tilt_error", "pan_centered", "tilt_centered", "locked",
            "detections", "vision_frame_width", "vision_frame_height",
            "inference_ms", "detection_fps",
        }
        self.assertTrue(required_vision_fields.issubset(status))
        self.assertFalse(status["vision_enabled"])
        self.assertEqual(status["tracking_state"], "SEARCHING")

        started = self.client.post("/api/record/start")
        self.assertEqual(started.status_code, 200)
        self.assertTrue(started.get_json()["status"]["recording"])
        captured = self.client.post("/api/capture")
        self.assertEqual(captured.status_code, 200)
        self.assertTrue(captured.get_json()["capture_path"].endswith(".jpg"))
        stopped = self.client.post("/api/record/stop")
        self.assertEqual(stopped.status_code, 200)
        self.assertFalse(stopped.get_json()["status"]["recording"])

        enabled = self.client.post("/api/dataset/start").get_json()
        self.assertTrue(enabled["status"]["dataset_capture_enabled"])
        disabled = self.client.post("/api/dataset/stop").get_json()
        self.assertFalse(disabled["status"]["dataset_capture_enabled"])

        vision_started = self.client.post("/api/vision/start")
        self.assertEqual(vision_started.status_code, 200)
        self.assertTrue(vision_started.get_json()["status"]["vision_enabled"])
        self.assertFalse(self.client.post("/api/vision/start").get_json()["changed"])
        vision_stopped = self.client.post("/api/vision/stop")
        self.assertEqual(vision_stopped.status_code, 200)
        self.assertFalse(vision_stopped.get_json()["status"]["vision_enabled"])

    def test_stream_has_multipart_jpeg_framing(self):
        self.service.start()
        response = self.client.get("/stream", buffered=False)
        try:
            chunk = next(response.response)
        finally:
            response.close()
        self.assertEqual(response.status_code, 200)
        self.assertIn(b"--FRAME\r\n", chunk)
        self.assertIn(b"Content-Type: image/jpeg", chunk)
        self.assertIn(b"fake-jpeg", chunk)

    def test_unavailable_camera_and_low_storage_error_codes(self):
        unavailable = self.client.post("/api/capture")
        self.assertEqual(unavailable.status_code, 503)
        self.assertFalse(unavailable.get_json()["ok"])

        self.service.start()
        self.free_bytes = 1
        low_storage = self.client.post("/api/record/start")
        self.assertEqual(low_storage.status_code, 507)
        self.assertIn("insufficient free space", low_storage.get_json()["error"])

    def test_vision_start_error_codes_leave_stream_available(self):
        self.service.start()

        unconfigured = self._new_vision(None)
        unconfigured.start()
        self.addCleanup(unconfigured.close)
        unconfigured_client = create_app(self.service, unconfigured).test_client()
        response = unconfigured_client.post("/api/vision/start")
        self.assertEqual(response.status_code, 400)
        self.assertFalse(response.get_json()["status"]["vision_enabled"])

        missing_path = self._new_vision(
            Path(self.temporary_directory.name) / "missing.pt"
        )
        missing_path.start()
        self.addCleanup(missing_path.close)
        missing_path_client = create_app(self.service, missing_path).test_client()
        response = missing_path_client.post("/api/vision/start")
        self.assertEqual(response.status_code, 400)
        self.assertIn("does not exist", response.get_json()["error"])

        absent_class = self._new_vision(
            Path(self.temporary_directory.name) / "absent-class.pt",
            detector_factory=lambda *_args: AbsentClassDetector(),
        )
        absent_class.start()
        self.addCleanup(absent_class.close)
        absent_class_client = create_app(self.service, absent_class).test_client()
        response = absent_class_client.post("/api/vision/start")
        self.assertEqual(response.status_code, 400)
        self.assertIn("target class is absent", response.get_json()["error"])

        failing = self._new_vision(
            Path(self.temporary_directory.name) / "failing.pt",
            detector_factory=lambda *_args: (_ for _ in ()).throw(
                RuntimeError("model load failed")
            ),
        )
        failing.start()
        self.addCleanup(failing.close)
        failing_client = create_app(self.service, failing).test_client()
        response = failing_client.post("/api/vision/start")
        self.assertEqual(response.status_code, 503)
        self.assertIn("model load failed", response.get_json()["error"])

        stream = failing_client.get("/stream", buffered=False)
        try:
            self.assertIn(b"fake-jpeg", next(stream.response))
        finally:
            stream.close()


if __name__ == "__main__":
    unittest.main()
