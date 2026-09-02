#!/usr/bin/env python3
"""PC-only tests for YOLO parsing, target selection, and tracking state."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from host.vision.detector import Detection, DetectorConfigurationError, YoloDetector
from host.vision.target_selector import TargetSelector
from host.vision.tracking_state import TrackingState, TrackingStateMachine
from host.camera.stream_server import build_parser


def detection(
    confidence: float,
    *,
    class_name: str = "animal_mouse",
    x1: float = 40,
    y1: float = 40,
    x2: float = 60,
    y2: float = 60,
) -> Detection:
    return Detection(0, class_name, confidence, x1, y1, x2, y2)


class FakeCv2:
    INTER_AREA = 1
    COLOR_RGB2BGR = 2

    @staticmethod
    def resize(frame, dimensions, interpolation):
        del interpolation
        width, height = dimensions
        resized = np.empty((height, width, 3), dtype=frame.dtype)
        resized[:] = frame[0, 0]
        return resized

    @staticmethod
    def cvtColor(frame, conversion):
        if conversion != FakeCv2.COLOR_RGB2BGR:
            raise AssertionError("unexpected color conversion")
        return frame[..., ::-1].copy()


class FakeModel:
    names = {0: "animal_mouse", 1: "cat"}

    def __init__(self):
        self.arguments = None

    def predict(self, **arguments):
        self.arguments = arguments
        boxes = SimpleNamespace(
            xyxy=np.array([[5, 2.5, 25, 12.5], [0, 0, 5, 5]], dtype=float),
            conf=np.array([0.9, 0.2], dtype=float),
            cls=np.array([0, 1], dtype=float),
            id=np.array([7, 8], dtype=float),
        )
        return [SimpleNamespace(boxes=boxes)]


class DetectorTests(unittest.TestCase):
    def test_model_mapping_bgr_conversion_and_scaled_parsing(self):
        with tempfile.TemporaryDirectory() as directory:
            model_path = Path(directory) / "model.pt"
            model_path.touch()
            model = FakeModel()
            with patch.dict(sys.modules, {"cv2": FakeCv2}):
                detector = YoloDetector(
                    model_path,
                    confidence=0.5,
                    inference_size=50,
                    model_loader=lambda _path: model,
                )
                detector.validate_target_class("animal_mouse")
                frame = np.empty((50, 100, 3), dtype=np.uint8)
                frame[:] = (10, 20, 30)
                detections = detector.detect(frame)

        self.assertEqual(detector.class_names, {0: "animal_mouse", 1: "cat"})
        self.assertEqual(len(detections), 1)
        self.assertEqual(detections[0].tracking_id, 7)
        self.assertEqual(
            (detections[0].x1, detections[0].y1, detections[0].x2, detections[0].y2),
            (10.0, 5.0, 50.0, 25.0),
        )
        self.assertEqual(tuple(model.arguments["source"][0, 0]), (30, 20, 10))
        self.assertEqual(model.arguments["conf"], 0.5)
        self.assertEqual(model.arguments["imgsz"], 50)

    def test_missing_model_class_is_a_configuration_error(self):
        with tempfile.TemporaryDirectory() as directory:
            model_path = Path(directory) / "model.pt"
            model_path.touch()
            detector = YoloDetector(
                model_path,
                confidence=0.5,
                inference_size=640,
                model_loader=lambda _path: FakeModel(),
            )
            with self.assertRaisesRegex(DetectorConfigurationError, "not present"):
                detector.validate_target_class("dog")


class TargetSelectorTests(unittest.TestCase):
    def test_exact_class_threshold_confidence_and_stable_tie(self):
        first_tie = detection(0.9, x1=1, x2=2)
        second_tie = detection(0.9, x1=3, x2=4)
        candidates = (
            detection(0.99, class_name="cat"),
            detection(0.49),
            first_tie,
            second_tie,
        )
        selector = TargetSelector("animal_mouse", 0.5)
        self.assertIs(selector.select(candidates), first_tie)
        self.assertIsNone(selector.select((detection(0.49),)))


class TrackingStateTests(unittest.TestCase):
    def test_error_signs_inclusive_dead_zone_and_complete_sequence(self):
        tracker = TrackingStateMachine(dead_zone_x=0.1, dead_zone_y=0.2)
        above_left = detection(0.8, x1=0, x2=20, y1=0, y2=20)
        acquired = tracker.update(
            (above_left,), above_left, frame_width=100, frame_height=100
        )
        self.assertEqual(acquired.state, TrackingState.DETECTED)
        self.assertLess(acquired.pan_error, 0)
        self.assertLess(acquired.tilt_error, 0)

        dead_zone_edge = detection(0.8, x1=50, x2=60, y1=55, y2=65)
        locked = tracker.update(
            (dead_zone_edge,), dead_zone_edge, frame_width=100, frame_height=100
        )
        self.assertAlmostEqual(locked.pan_error, 0.1)
        self.assertAlmostEqual(locked.tilt_error, 0.2)
        self.assertTrue(locked.pan_centered)
        self.assertTrue(locked.tilt_centered)
        self.assertTrue(locked.locked)
        self.assertEqual(locked.state, TrackingState.LOCKED)

        below_right = detection(0.8, x1=80, x2=100, y1=80, y2=100)
        tracking = tracker.update(
            (below_right,), below_right, frame_width=100, frame_height=100
        )
        self.assertEqual(tracking.state, TrackingState.TRACKING)
        self.assertGreater(tracking.pan_error, 0)
        self.assertGreater(tracking.tilt_error, 0)

        lost = tracker.update(
            (detection(0.9, class_name="cat"),),
            None,
            frame_width=100,
            frame_height=100,
        )
        self.assertEqual(lost.state, TrackingState.SEARCHING)
        self.assertEqual(lost.detections, ())
        self.assertIsNone(lost.pan_error)
        self.assertFalse(lost.locked)

        reacquired = tracker.update(
            (above_left,), above_left, frame_width=100, frame_height=100
        )
        self.assertEqual(reacquired.state, TrackingState.DETECTED)


class InterfaceTests(unittest.TestCase):
    def test_cli_vision_defaults_and_forbidden_dependencies(self):
        arguments = build_parser().parse_args([])
        self.assertIsNone(arguments.model)
        self.assertEqual(arguments.target_class, "animal_mouse")
        self.assertEqual(arguments.confidence, 0.5)
        self.assertEqual(arguments.detection_fps, 5.0)
        self.assertEqual(arguments.inference_size, 640)
        self.assertEqual(arguments.dead_zone_x, 0.05)
        self.assertEqual(arguments.dead_zone_y, 0.05)

        source_directory = Path(__file__).resolve().parents[1] / "host" / "vision"
        source = "\n".join(
            path.read_text(encoding="utf-8")
            for path in source_directory.glob("*.py")
        )
        self.assertNotIn("host.sentry_mcu", source)
        self.assertNotIn("VideoCapture", source)
        self.assertNotIn("Picamera2", source)


if __name__ == "__main__":
    unittest.main()
