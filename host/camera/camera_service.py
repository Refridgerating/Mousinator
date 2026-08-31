"""Thread-safe camera orchestration independent of the HTTP transport."""

from __future__ import annotations

import logging
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping, Protocol

from .storage import CaptureRecord, MediaStorage, StorageLowError


class CameraUnavailableError(RuntimeError):
    """Raised when an operation requires a camera that is not running."""


@dataclass(frozen=True)
class CapturedFrame:
    array: Any
    metadata: Mapping[str, Any]


class CameraBackend(Protocol):
    native_resolution: tuple[int, int] | None

    def start(self) -> None: ...

    def stop(self) -> None: ...

    def get_frame(self) -> CapturedFrame: ...

    def capture_sensor_native(self) -> CapturedFrame: ...

    def start_recording(self, path: Path) -> None: ...

    def stop_recording(self) -> None: ...

    def wait_for_jpeg(
        self, after_sequence: int, timeout_s: float
    ) -> tuple[int, bytes] | None: ...


class CameraService:
    """Own one camera backend and coordinate all camera consumers."""

    def __init__(
        self,
        backend: CameraBackend,
        storage: MediaStorage,
        *,
        width: int,
        height: int,
        fps: float,
        capture_interval_s: float,
        logger: logging.Logger | None = None,
    ) -> None:
        if width <= 0 or height <= 0:
            raise ValueError("camera dimensions must be positive")
        if fps <= 0:
            raise ValueError("fps must be positive")
        if capture_interval_s <= 0:
            raise ValueError("capture interval must be positive")

        self._backend = backend
        self._storage = storage
        self._width = width
        self._height = height
        self._fps = fps
        self._capture_interval_s = capture_interval_s
        self._logger = logger or logging.getLogger(__name__)

        self._state_lock = threading.RLock()
        self._operation_lock = threading.Lock()
        self._dataset_condition = threading.Condition(self._state_lock)
        self._camera_running = False
        self._recording = False
        self._recording_path: Path | None = None
        self._dataset_enabled = False
        self._last_error: str | None = None
        self._shutdown = False
        self._dataset_thread: threading.Thread | None = None

    def start(self) -> None:
        with self._operation_lock:
            with self._state_lock:
                if self._camera_running:
                    return
                if self._shutdown:
                    raise CameraUnavailableError("camera service has been closed")
            try:
                self._backend.start()
            except Exception as exc:
                self._set_last_error(str(exc))
                raise CameraUnavailableError(f"camera failed to start: {exc}") from exc

            with self._state_lock:
                self._camera_running = True
                self._last_error = None
                self._dataset_thread = threading.Thread(
                    target=self._dataset_worker,
                    name="camera-dataset-capture",
                    daemon=False,
                )
                self._dataset_thread.start()
        self._logger.info(
            "camera started at %dx%d, %.2f FPS",
            self._width,
            self._height,
            self._fps,
        )

    def close(self) -> None:
        with self._dataset_condition:
            if self._shutdown:
                return
            self._shutdown = True
            self._dataset_enabled = False
            self._dataset_condition.notify_all()

        dataset_thread = self._dataset_thread
        if dataset_thread is not None and dataset_thread is not threading.current_thread():
            dataset_thread.join()

        stop_error: Exception | None = None
        with self._operation_lock:
            if self._recording:
                try:
                    self._backend.stop_recording()
                    self._logger.info("recording stopped during shutdown")
                except Exception as exc:  # continue releasing the camera
                    stop_error = exc
                    self._set_last_error(str(exc))
                finally:
                    with self._state_lock:
                        self._recording = False
                        self._recording_path = None

            with self._state_lock:
                was_running = self._camera_running
            if was_running:
                try:
                    self._backend.stop()
                except Exception as exc:
                    stop_error = stop_error or exc
                    self._set_last_error(str(exc))
                finally:
                    with self._state_lock:
                        self._camera_running = False

        if was_running:
            self._logger.info("camera stopped")
        if stop_error is not None:
            raise RuntimeError(f"camera shutdown failed: {stop_error}") from stop_error

    def get_frame(self, *, copy: bool = True) -> Any:
        with self._operation_lock:
            self._require_running()
            captured = self._backend.get_frame()
            if copy and hasattr(captured.array, "copy"):
                return captured.array.copy()
            return captured.array

    def start_recording(self) -> Path:
        with self._operation_lock:
            self._require_running()
            with self._state_lock:
                if self._recording and self._recording_path is not None:
                    return self._recording_path

            self._storage.ensure_free_space()
            path = self._storage.allocate_recording_path()
            try:
                self._backend.start_recording(path)
            except Exception as exc:
                self._set_last_error(str(exc))
                raise

            with self._state_lock:
                self._recording = True
                self._recording_path = path
                self._last_error = None
            self._logger.info("recording started: %s", path)
            return path

    def stop_recording(self) -> Path | None:
        with self._operation_lock:
            self._require_running()
            with self._state_lock:
                if not self._recording:
                    return None
                path = self._recording_path
            try:
                self._backend.stop_recording()
            except Exception as exc:
                self._set_last_error(str(exc))
                raise

            with self._state_lock:
                self._recording = False
                self._recording_path = None
                self._last_error = None
            self._logger.info("recording stopped: %s", path)
            return path

    def capture(self, capture_mode: str = "manual") -> CaptureRecord:
        with self._operation_lock:
            return self._capture_locked(capture_mode)

    def start_dataset_capture(self) -> bool:
        with self._dataset_condition:
            self._require_running_locked()
            changed = not self._dataset_enabled
            self._dataset_enabled = True
            self._last_error = None
            self._dataset_condition.notify_all()
        if changed:
            self._logger.info(
                "dataset capture enabled at %.3f second intervals",
                self._capture_interval_s,
            )
        return changed

    def stop_dataset_capture(self) -> bool:
        with self._dataset_condition:
            changed = self._dataset_enabled
            self._dataset_enabled = False
            self._dataset_condition.notify_all()

        # Wait for an in-flight interval capture so none complete after this returns.
        with self._operation_lock:
            pass
        if changed:
            self._logger.info("dataset capture disabled")
        return changed

    def wait_for_jpeg(
        self, after_sequence: int, timeout_s: float = 10.0
    ) -> tuple[int, bytes] | None:
        self._require_running()
        return self._backend.wait_for_jpeg(after_sequence, timeout_s)

    def status(self) -> dict[str, Any]:
        with self._state_lock:
            recording_path = self._recording_path
            state = {
                "camera_running": self._camera_running,
                "recording": self._recording,
                "recording_filename": (
                    None if recording_path is None else recording_path.name
                ),
                "dataset_capture_enabled": self._dataset_enabled,
                "capture_interval": self._capture_interval_s,
                "resolution": {"width": self._width, "height": self._height},
                "stream_resolution": {"width": self._width, "height": self._height},
                "native_resolution": self._native_resolution_status(),
                "fps": self._fps,
                "minimum_free_bytes": self._storage.min_free_bytes,
                "last_error": self._last_error,
            }
        try:
            state["free_bytes"] = self._storage.free_bytes()
        except OSError as exc:
            state["free_bytes"] = None
            state["last_error"] = state["last_error"] or str(exc)
        return state

    def relative_media_path(self, path: Path) -> str:
        return path.resolve().relative_to(self._storage.data_dir).as_posix()

    def _dataset_worker(self) -> None:
        while True:
            with self._dataset_condition:
                self._dataset_condition.wait_for(
                    lambda: self._shutdown or self._dataset_enabled
                )
                if self._shutdown:
                    return

            try:
                with self._operation_lock:
                    with self._state_lock:
                        if self._shutdown:
                            return
                        if not self._dataset_enabled:
                            continue
                    self._capture_locked("interval")
            except StorageLowError as exc:
                with self._dataset_condition:
                    self._dataset_enabled = False
                    self._last_error = str(exc)
                self._logger.warning(
                    "dataset capture disabled because storage is low: %s", exc
                )
                continue
            except CameraUnavailableError:
                return
            except Exception as exc:
                self._logger.error("interval dataset capture failed: %s", exc)

            with self._dataset_condition:
                if self._shutdown:
                    return
                if self._dataset_enabled:
                    self._dataset_condition.wait_for(
                        lambda: self._shutdown or not self._dataset_enabled,
                        timeout=self._capture_interval_s,
                    )

    def _capture_locked(self, capture_mode: str) -> CaptureRecord:
        self._require_running()
        self._storage.ensure_free_space()
        with self._state_lock:
            recording = self._recording

        try:
            if recording:
                captured = self._backend.get_frame()
                source_mode = "video_frame"
            else:
                captured = self._backend.capture_sensor_native()
                source_mode = "sensor_native"
            record = self._storage.save_capture(
                captured.array,
                capture_mode=capture_mode,
                source_mode=source_mode,
                camera_metadata=captured.metadata,
            )
        except Exception as exc:
            self._set_last_error(str(exc))
            raise

        self._set_last_error(None)
        self._logger.info(
            "frame saved: %s (%s, %s)",
            record.path,
            capture_mode,
            source_mode,
        )
        return record

    def _require_running(self) -> None:
        with self._state_lock:
            self._require_running_locked()

    def _require_running_locked(self) -> None:
        if not self._camera_running or self._shutdown:
            raise CameraUnavailableError("camera is not running")

    def _set_last_error(self, message: str | None) -> None:
        with self._state_lock:
            self._last_error = message

    def _native_resolution_status(self) -> dict[str, int] | None:
        native = self._backend.native_resolution
        if native is None:
            return None
        return {"width": native[0], "height": native[1]}
