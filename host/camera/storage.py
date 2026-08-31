"""Filesystem storage for camera recordings and dataset captures."""

from __future__ import annotations

import csv
import os
import shutil
import threading
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Callable, Mapping


GIB = 1024 * 1024 * 1024
CAPTURE_METADATA_FIELDS = (
    "filename",
    "timestamp",
    "width",
    "height",
    "capture_mode",
    "source_mode",
    "exposure_time_us",
    "analogue_gain",
)


class StorageLowError(RuntimeError):
    """Raised when a media write would violate the free-space threshold."""

    def __init__(self, free_bytes: int, minimum_bytes: int) -> None:
        self.free_bytes = free_bytes
        self.minimum_bytes = minimum_bytes
        super().__init__(
            "insufficient free space: "
            f"{free_bytes / GIB:.2f} GiB available, "
            f"{minimum_bytes / GIB:.2f} GiB required"
        )


@dataclass(frozen=True)
class CaptureRecord:
    path: Path
    relative_path: str
    filename: str
    timestamp: str
    width: int
    height: int
    capture_mode: str
    source_mode: str


class MediaStorage:
    """Own camera media paths and serialized metadata writes."""

    def __init__(
        self,
        data_dir: Path,
        min_free_bytes: int,
        *,
        disk_usage: Callable[[Path], Any] = shutil.disk_usage,
    ) -> None:
        if min_free_bytes < 0:
            raise ValueError("min_free_bytes must not be negative")

        self.data_dir = Path(data_dir).resolve()
        self.recordings_dir = self.data_dir / "recordings"
        self.captures_dir = self.data_dir / "captures"
        self.raw_captures_dir = self.captures_dir / "raw"
        self.metadata_path = self.captures_dir / "capture_metadata.csv"
        self.min_free_bytes = min_free_bytes
        self._disk_usage = disk_usage
        self._lock = threading.Lock()

        self.recordings_dir.mkdir(parents=True, exist_ok=True)
        self.raw_captures_dir.mkdir(parents=True, exist_ok=True)

    def free_bytes(self) -> int:
        return int(self._disk_usage(self.data_dir).free)

    def ensure_free_space(self) -> int:
        free_bytes = self.free_bytes()
        if free_bytes < self.min_free_bytes:
            raise StorageLowError(free_bytes, self.min_free_bytes)
        return free_bytes

    def allocate_recording_path(self, *, now: datetime | None = None) -> Path:
        """Return a collision-free MP4 path without overwriting existing media."""
        timestamp = self._normalize_timestamp(now)
        stem = f"sentry_{timestamp:%Y-%m-%d_%H%M%S}"
        with self._lock:
            return self._unique_path(self.recordings_dir, stem, ".mp4")

    def save_capture(
        self,
        frame: Any,
        *,
        capture_mode: str,
        source_mode: str,
        camera_metadata: Mapping[str, Any] | None = None,
        now: datetime | None = None,
    ) -> CaptureRecord:
        if capture_mode not in ("manual", "interval"):
            raise ValueError("capture_mode must be manual or interval")
        if source_mode not in ("sensor_native", "video_frame"):
            raise ValueError("source_mode must be sensor_native or video_frame")
        if getattr(frame, "ndim", 0) < 2:
            raise ValueError("camera frame must have at least two dimensions")

        timestamp = self._normalize_timestamp(now)
        height = int(frame.shape[0])
        width = int(frame.shape[1])
        stem = timestamp.strftime("%Y%m%d_%H%M%S_") + f"{timestamp.microsecond // 1000:03d}"
        metadata = camera_metadata or {}

        with self._lock:
            self.ensure_free_space()
            metadata_exists = self._validate_metadata_header()
            path = self._unique_path(self.raw_captures_dir, stem, ".jpg")
            temporary_path = path.with_name(path.name + ".part")
            self._write_jpeg(frame, temporary_path)
            os.replace(temporary_path, path)

            record = CaptureRecord(
                path=path,
                relative_path=path.relative_to(self.data_dir).as_posix(),
                filename=path.name,
                timestamp=timestamp.isoformat(timespec="milliseconds"),
                width=width,
                height=height,
                capture_mode=capture_mode,
                source_mode=source_mode,
            )
            self._append_metadata(record, metadata, metadata_exists)
            return record

    @staticmethod
    def _normalize_timestamp(value: datetime | None) -> datetime:
        timestamp = value or datetime.now().astimezone()
        if timestamp.tzinfo is None:
            timestamp = timestamp.astimezone()
        return timestamp

    @staticmethod
    def _unique_path(directory: Path, stem: str, suffix: str) -> Path:
        candidate = directory / f"{stem}{suffix}"
        counter = 1
        while candidate.exists() or candidate.with_name(candidate.name + ".part").exists():
            candidate = directory / f"{stem}_{counter:03d}{suffix}"
            counter += 1
        return candidate

    @staticmethod
    def _write_jpeg(frame: Any, path: Path) -> None:
        try:
            from PIL import Image
        except ImportError as exc:  # pragma: no cover - Pi dependency failure
            raise RuntimeError("Pillow is required to save camera captures") from exc

        try:
            Image.fromarray(frame).save(path, format="JPEG", quality=95)
        except Exception:
            try:
                path.unlink(missing_ok=True)
            except OSError:
                pass
            raise

    def _validate_metadata_header(self) -> bool:
        exists = self.metadata_path.exists()
        if exists:
            with self.metadata_path.open("r", newline="", encoding="utf-8") as metadata_file:
                header = next(csv.reader(metadata_file), None)
            if header != list(CAPTURE_METADATA_FIELDS):
                raise RuntimeError(
                    f"unexpected capture metadata header in {self.metadata_path}"
                )
        return exists

    def _append_metadata(
        self,
        record: CaptureRecord,
        camera_metadata: Mapping[str, Any],
        metadata_exists: bool,
    ) -> None:
        row = {
            "filename": record.filename,
            "timestamp": record.timestamp,
            "width": record.width,
            "height": record.height,
            "capture_mode": record.capture_mode,
            "source_mode": record.source_mode,
            "exposure_time_us": camera_metadata.get("ExposureTime", ""),
            "analogue_gain": camera_metadata.get("AnalogueGain", ""),
        }
        with self.metadata_path.open("a", newline="", encoding="utf-8") as metadata_file:
            writer = csv.DictWriter(metadata_file, fieldnames=CAPTURE_METADATA_FIELDS)
            if not metadata_exists:
                writer.writeheader()
            writer.writerow(row)
            metadata_file.flush()
            os.fsync(metadata_file.fileno())
