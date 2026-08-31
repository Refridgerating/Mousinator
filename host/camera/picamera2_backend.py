"""Picamera2 implementation kept separate from PC-testable service logic."""

from __future__ import annotations

import io
import logging
import threading
from pathlib import Path
from typing import Any

from .camera_service import CapturedFrame


class JpegFrameBuffer(io.BufferedIOBase):
    """Publish complete encoder writes to any number of HTTP consumers."""

    def __init__(self) -> None:
        super().__init__()
        self._condition = threading.Condition()
        self._sequence = 0
        self._frame: bytes | None = None
        self._active = False
        self._closed = False

    def writable(self) -> bool:
        return True

    def write(self, data: bytes | bytearray | memoryview) -> int:
        frame = bytes(data)
        with self._condition:
            if not self._active:
                return len(frame)
            self._frame = frame
            self._sequence += 1
            self._condition.notify_all()
        return len(frame)

    def activate(self) -> None:
        with self._condition:
            self._closed = False
            self._active = True
            self._condition.notify_all()

    def deactivate(self) -> None:
        with self._condition:
            self._active = False

    def shutdown(self) -> None:
        with self._condition:
            self._active = False
            self._closed = True
            self._condition.notify_all()

    def wait_for_frame(
        self, after_sequence: int, timeout_s: float
    ) -> tuple[int, bytes] | None:
        with self._condition:
            ready = self._condition.wait_for(
                lambda: self._sequence > after_sequence or self._closed,
                timeout=timeout_s,
            )
            if not ready or self._sequence <= after_sequence or self._frame is None:
                return None
            return self._sequence, self._frame


class Picamera2Backend:
    """Own the physical CSI camera and all Picamera2 encoders."""

    def __init__(
        self,
        *,
        width: int,
        height: int,
        fps: float,
        logger: logging.Logger | None = None,
    ) -> None:
        self._width = width
        self._height = height
        self._fps = fps
        self._logger = logger or logging.getLogger(__name__)
        self._frame_buffer = JpegFrameBuffer()

        self.native_resolution: tuple[int, int] | None = None
        self._picam2: Any = None
        self._video_config: Any = None
        self._still_config: Any = None
        self._stream_encoder: Any = None
        self._stream_output: Any = None
        self._record_encoder: Any = None
        self._record_output: Any = None
        self._running = False

    def start(self) -> None:
        if self._running:
            return
        try:
            from picamera2 import Picamera2
        except ImportError as exc:  # pragma: no cover - Raspberry Pi dependency
            raise RuntimeError(
                "Picamera2 is unavailable; install python3-picamera2 on Raspberry Pi OS"
            ) from exc

        picam2 = Picamera2()
        try:
            self.native_resolution = self._largest_sensor_resolution(picam2)
            self._video_config = picam2.create_video_configuration(
                # libcamera calls this BGR888, but Picamera2 exposes its array
                # bytes in R/G/B order, which is the public get_frame contract.
                main={"size": (self._width, self._height), "format": "BGR888"},
                lores={"size": (self._width, self._height), "format": "YUV420"},
                controls={"FrameRate": self._fps},
                buffer_count=6,
            )
            still_main: dict[str, Any] = {"format": "BGR888"}
            if self.native_resolution is not None:
                still_main["size"] = self.native_resolution
            self._still_config = picam2.create_still_configuration(
                main=still_main,
                buffer_count=2,
            )
            picam2.configure(self._video_config)
            picam2.start()
            self._picam2 = picam2
            self._running = True
            self._start_stream_encoder()
        except Exception:
            try:
                picam2.stop()
            except Exception:
                pass
            try:
                picam2.close()
            except Exception:
                pass
            self._picam2 = None
            self._running = False
            raise

    def stop(self) -> None:
        if self._picam2 is None:
            return

        error: Exception | None = None
        if self._record_encoder is not None:
            try:
                self.stop_recording()
            except Exception as exc:
                error = exc
        try:
            self._stop_stream_encoder()
        except Exception as exc:
            error = error or exc
        try:
            if self._running:
                self._picam2.stop()
        except Exception as exc:
            error = error or exc
        try:
            self._picam2.close()
        except Exception as exc:
            error = error or exc
        finally:
            self._running = False
            self._picam2 = None
            self._video_config = None
            self._still_config = None
            self._frame_buffer.shutdown()

        if error is not None:
            raise error

    def get_frame(self) -> CapturedFrame:
        self._require_running()
        arrays, metadata = self._picam2.capture_arrays(["main"])
        return CapturedFrame(arrays[0], metadata)

    def capture_sensor_native(self) -> CapturedFrame:
        self._require_running()
        self._stop_stream_encoder()
        capture_error: Exception | None = None
        captured: CapturedFrame | None = None
        try:
            arrays, metadata = self._picam2.switch_mode_and_capture_arrays(
                self._still_config,
                ["main"],
            )
            captured = CapturedFrame(arrays[0], metadata)
        except Exception as exc:
            capture_error = exc
        try:
            self._start_stream_encoder()
        except Exception as exc:
            if capture_error is None:
                capture_error = exc
            else:
                self._logger.error(
                    "MJPEG encoder failed to restart after still-capture error: %s",
                    exc,
                )
        if capture_error is not None:
            raise capture_error
        assert captured is not None
        return captured

    def start_recording(self, path: Path) -> None:
        self._require_running()
        if self._record_encoder is not None:
            return
        try:
            from picamera2.encoders import H264Encoder
            from picamera2.outputs import PyavOutput
        except ImportError as exc:  # pragma: no cover - Raspberry Pi dependency
            raise RuntimeError(
                "PyAV recording support is unavailable; install python3-av"
            ) from exc

        encoder = H264Encoder()
        output = PyavOutput(str(path))
        try:
            self._picam2.start_encoder(encoder, output, name="main")
        except Exception:
            try:
                output.stop()
            except Exception:
                pass
            raise
        self._record_encoder = encoder
        self._record_output = output

    def stop_recording(self) -> None:
        if self._record_encoder is None:
            return
        encoder = self._record_encoder
        try:
            self._picam2.stop_encoder(encoder)
        finally:
            self._record_encoder = None
            self._record_output = None

    def wait_for_jpeg(
        self, after_sequence: int, timeout_s: float
    ) -> tuple[int, bytes] | None:
        return self._frame_buffer.wait_for_frame(after_sequence, timeout_s)

    def _start_stream_encoder(self) -> None:
        self._require_running()
        if self._stream_encoder is not None:
            return
        try:
            from picamera2.encoders import MJPEGEncoder
            from picamera2.outputs import FileOutput
        except ImportError as exc:  # pragma: no cover - Raspberry Pi dependency
            raise RuntimeError("Picamera2 MJPEG support is unavailable") from exc

        encoder = MJPEGEncoder()
        output = FileOutput(self._frame_buffer)
        self._frame_buffer.activate()
        try:
            self._picam2.start_encoder(encoder, output, name="lores")
        except Exception:
            self._frame_buffer.deactivate()
            raise
        self._stream_encoder = encoder
        self._stream_output = output

    def _stop_stream_encoder(self) -> None:
        if self._stream_encoder is None:
            return
        encoder = self._stream_encoder
        self._frame_buffer.deactivate()
        try:
            self._picam2.stop_encoder(encoder)
        finally:
            self._stream_encoder = None
            self._stream_output = None

    def _require_running(self) -> None:
        if not self._running or self._picam2 is None:
            raise RuntimeError("Picamera2 camera is not running")

    @staticmethod
    def _largest_sensor_resolution(picam2: Any) -> tuple[int, int] | None:
        resolutions = []
        for mode in getattr(picam2, "sensor_modes", ()):
            size = mode.get("size")
            if (
                isinstance(size, (tuple, list))
                and len(size) == 2
                and int(size[0]) > 0
                and int(size[1]) > 0
            ):
                resolutions.append((int(size[0]), int(size[1])))
        if resolutions:
            return max(resolutions, key=lambda size: size[0] * size[1])
        sensor_resolution = getattr(picam2, "sensor_resolution", None)
        if sensor_resolution is None:
            return None
        return int(sensor_resolution[0]), int(sensor_resolution[1])
