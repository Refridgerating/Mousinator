#!/usr/bin/env python3
"""Host tests for camera media paths, disk protection, and metadata."""

from __future__ import annotations

import csv
import sys
import tempfile
import unittest
from collections import namedtuple
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from host.camera.storage import MediaStorage, StorageLowError


DiskUsage = namedtuple("DiskUsage", "total used free")


class MediaStorageTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.data_dir = Path(self.temporary_directory.name) / "data"
        self.free_bytes = 4 * 1024 * 1024 * 1024
        self.storage = MediaStorage(
            self.data_dir,
            1024 * 1024 * 1024,
            disk_usage=lambda _path: DiskUsage(
                8 * 1024 * 1024 * 1024,
                4 * 1024 * 1024 * 1024,
                self.free_bytes,
            ),
        )
        self.now = datetime(2026, 8, 30, 23, 5, 1, 123456, timezone.utc)

    def test_paths_are_created_and_recording_names_do_not_collide(self):
        self.assertTrue(self.storage.recordings_dir.is_dir())
        self.assertTrue(self.storage.raw_captures_dir.is_dir())
        first = self.storage.allocate_recording_path(now=self.now)
        self.assertEqual(first.name, "sentry_2026-08-30_230501.mp4")
        first.touch()
        second = self.storage.allocate_recording_path(now=self.now)
        self.assertEqual(second.name, "sentry_2026-08-30_230501_001.mp4")

    def test_capture_writes_jpeg_and_complete_metadata(self):
        frame = np.zeros((4, 6, 3), dtype=np.uint8)
        record = self.storage.save_capture(
            frame,
            capture_mode="manual",
            source_mode="sensor_native",
            camera_metadata={"ExposureTime": 1234, "AnalogueGain": 1.5},
            now=self.now,
        )
        self.assertEqual(record.filename, "20260830_230501_123.jpg")
        self.assertEqual(record.relative_path, "captures/raw/20260830_230501_123.jpg")
        self.assertTrue(record.path.is_file())
        self.assertEqual((record.width, record.height), (6, 4))

        with self.storage.metadata_path.open(newline="", encoding="utf-8") as file:
            rows = list(csv.DictReader(file))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["filename"], record.filename)
        self.assertEqual(rows[0]["capture_mode"], "manual")
        self.assertEqual(rows[0]["source_mode"], "sensor_native")
        self.assertEqual(rows[0]["exposure_time_us"], "1234")
        self.assertEqual(rows[0]["analogue_gain"], "1.5")

    def test_same_millisecond_capture_adds_suffix(self):
        frame = np.zeros((2, 2, 3), dtype=np.uint8)
        first = self.storage.save_capture(
            frame,
            capture_mode="interval",
            source_mode="video_frame",
            now=self.now,
        )
        second = self.storage.save_capture(
            frame,
            capture_mode="interval",
            source_mode="video_frame",
            now=self.now,
        )
        self.assertNotEqual(first.path, second.path)
        self.assertEqual(second.path.stem, first.path.stem + "_001")

    def test_low_space_rejects_writes_without_deleting_existing_files(self):
        existing = self.storage.recordings_dir / "keep.mp4"
        existing.write_bytes(b"keep")
        self.free_bytes = 512 * 1024 * 1024
        with self.assertRaises(StorageLowError):
            self.storage.ensure_free_space()
        with self.assertRaises(StorageLowError):
            self.storage.save_capture(
                np.zeros((2, 2, 3), dtype=np.uint8),
                capture_mode="manual",
                source_mode="video_frame",
            )
        self.assertEqual(existing.read_bytes(), b"keep")
        self.assertEqual(list(self.storage.raw_captures_dir.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
