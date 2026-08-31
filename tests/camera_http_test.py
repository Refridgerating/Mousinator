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
        self.app = create_app(self.service)
        self.client = self.app.test_client()
        self.addCleanup(self._close_service)

    def _close_service(self):
        try:
            self.service.close()
        except RuntimeError:
            pass

    def test_page_status_and_mutating_endpoints(self):
        self.service.start()
        page = self.client.get("/")
        self.assertEqual(page.status_code, 200)
        self.assertIn(b"Sentry Camera", page.data)

        status = self.client.get("/api/status").get_json()
        self.assertTrue(status["camera_running"])
        self.assertEqual(status["resolution"], {"width": 1280, "height": 720})
        self.assertEqual(status["fps"], 30)

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


if __name__ == "__main__":
    unittest.main()
