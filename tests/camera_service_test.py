#!/usr/bin/env python3
"""Host tests for camera lifecycle and concurrent dataset collection."""

from __future__ import annotations

import tempfile
import time
import unittest
import sys
from collections import namedtuple
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from host.camera.camera_service import (
    CameraService,
    CameraUnavailableError,
    CapturedFrame,
)
from host.camera.storage import MediaStorage


DiskUsage = namedtuple("DiskUsage", "total used free")


class FakeCameraBackend:
    native_resolution = (12, 8)

    def __init__(self):
        self.running = False
        self.start_count = 0
        self.stop_count = 0
        self.recording_paths = []
        self.recording = False
        self.video_capture_count = 0
        self.native_capture_count = 0
        self.video_frame = np.full((4, 6, 3), 7, dtype=np.uint8)
        self.native_frame = np.full((8, 12, 3), 9, dtype=np.uint8)

    def start(self):
        self.running = True
        self.start_count += 1

    def stop(self):
        self.running = False
        self.stop_count += 1

    def get_frame(self):
        self.video_capture_count += 1
        return CapturedFrame(
            self.video_frame,
            {"ExposureTime": 100, "AnalogueGain": 1.0},
        )

    def capture_sensor_native(self):
        self.native_capture_count += 1
        return CapturedFrame(
            self.native_frame,
            {"ExposureTime": 200, "AnalogueGain": 2.0},
        )

    def start_recording(self, path):
        self.recording = True
        self.recording_paths.append(path)

    def stop_recording(self):
        self.recording = False

    def wait_for_jpeg(self, after_sequence, timeout_s):
        del timeout_s
        return after_sequence + 1, b"jpeg"


class CameraServiceTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.free_bytes = 4 * 1024 * 1024 * 1024
        disk_usage = lambda _path: DiskUsage(
            8 * 1024 * 1024 * 1024,
            4 * 1024 * 1024 * 1024,
            self.free_bytes,
        )
        self.storage = MediaStorage(
            Path(self.temporary_directory.name) / "data",
            1024 * 1024 * 1024,
            disk_usage=disk_usage,
        )
        self.backend = FakeCameraBackend()
        self.service = CameraService(
            self.backend,
            self.storage,
            width=1280,
            height=720,
            fps=30,
            capture_interval_s=0.03,
        )
        self.addCleanup(self._close_service)

    def _close_service(self):
        try:
            self.service.close()
        except RuntimeError:
            pass

    def test_start_get_frame_copy_and_close(self):
        self.service.start()
        self.service.start()
        frame = self.service.get_frame()
        frame[0, 0, 0] = 99
        self.assertEqual(self.backend.video_frame[0, 0, 0], 7)
        self.assertEqual(self.backend.start_count, 1)
        self.assertTrue(self.service.status()["camera_running"])
        self.assertEqual(
            self.service.status()["native_resolution"],
            {"width": 12, "height": 8},
        )

        self.service.close()
        self.assertEqual(self.backend.stop_count, 1)
        with self.assertRaises(CameraUnavailableError):
            self.service.get_frame()

    def test_recording_is_idempotent_and_capture_uses_video_fallback(self):
        self.service.start()
        first_path = self.service.start_recording()
        second_path = self.service.start_recording()
        self.assertEqual(first_path, second_path)
        self.assertEqual(self.backend.recording_paths, [first_path])

        during_recording = self.service.capture("manual")
        self.assertEqual(during_recording.source_mode, "video_frame")
        self.assertEqual((during_recording.width, during_recording.height), (6, 4))

        stopped_path = self.service.stop_recording()
        self.assertEqual(stopped_path, first_path)
        self.assertIsNone(self.service.stop_recording())
        when_idle = self.service.capture("manual")
        self.assertEqual(when_idle.source_mode, "sensor_native")
        self.assertEqual((when_idle.width, when_idle.height), (12, 8))

    def test_interval_capture_is_immediate_and_stop_waits_for_worker(self):
        self.service.start()
        self.assertTrue(self.service.start_dataset_capture())
        deadline = time.monotonic() + 1.0
        while self.backend.native_capture_count < 2 and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertGreaterEqual(self.backend.native_capture_count, 2)
        self.assertFalse(self.service.start_dataset_capture())
        self.assertTrue(self.service.stop_dataset_capture())
        count_after_stop = self.backend.native_capture_count
        time.sleep(0.08)
        self.assertEqual(self.backend.native_capture_count, count_after_stop)
        self.assertFalse(self.service.status()["dataset_capture_enabled"])

    def test_low_space_disables_interval_capture(self):
        self.service.start()
        self.free_bytes = 100
        self.service.start_dataset_capture()
        deadline = time.monotonic() + 1.0
        while self.service.status()["dataset_capture_enabled"] and time.monotonic() < deadline:
            time.sleep(0.01)
        status = self.service.status()
        self.assertFalse(status["dataset_capture_enabled"])
        self.assertIn("insufficient free space", status["last_error"])
        self.assertEqual(self.backend.native_capture_count, 0)


if __name__ == "__main__":
    unittest.main()
