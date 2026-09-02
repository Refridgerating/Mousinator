"""No-queue vision worker consuming frames from CameraService."""

from __future__ import annotations

import logging
import threading
import time
from pathlib import Path
from typing import Any, Callable, Protocol

from host.camera.camera_service import CameraService, CameraUnavailableError

from .detector import (
    Detection,
    DetectorConfigurationError,
    DetectorInitializationError,
    YoloDetector,
)
from .target_selector import TargetSelector
from .tracking_state import TrackingSnapshot, TrackingStateMachine


class VisionConfigurationError(ValueError):
    """Raised when vision cannot be enabled with the supplied configuration."""


class VisionInitializationError(RuntimeError):
    """Raised when the configured model cannot be initialized."""


class Detector(Protocol):
    class_names: dict[int, str]

    def validate_target_class(self, target_class: str) -> None: ...

    def detect(self, rgb_frame: Any) -> tuple[Detection, ...]: ...


DetectorFactory = Callable[[Path, float, int], Detector]


class VisionService:
    """Run bounded-rate inference without owning or blocking camera acquisition."""

    def __init__(
        self,
        camera_service: CameraService,
        *,
        model_path: Path | None,
        target_class: str,
        confidence: float,
        detection_fps: float,
        inference_size: int,
        dead_zone_x: float,
        dead_zone_y: float,
        detector_factory: DetectorFactory | None = None,
        logger: logging.Logger | None = None,
    ) -> None:
        if not target_class:
            raise ValueError("target class must not be empty")
        if not 0.0 <= confidence <= 1.0:
            raise ValueError("confidence must be in the range 0..1")
        if detection_fps <= 0:
            raise ValueError("detection FPS must be positive")
        if inference_size <= 0:
            raise ValueError("inference size must be positive")

        self._camera_service = camera_service
        self._model_path = None if model_path is None else Path(model_path)
        self._target_class = target_class
        self._confidence = confidence
        self._detection_fps_limit = detection_fps
        self._inference_size = inference_size
        self._tracking = TrackingStateMachine(
            dead_zone_x=dead_zone_x,
            dead_zone_y=dead_zone_y,
        )
        self._selector = TargetSelector(target_class, confidence)
        self._detector_factory = detector_factory or self._create_detector
        self._logger = logger or logging.getLogger(__name__)

        self._state_lock = threading.RLock()
        self._condition = threading.Condition(self._state_lock)
        self._model_lock = threading.Lock()
        self._thread: threading.Thread | None = None
        self._detector: Detector | None = None
        self._enabled = False
        self._shutdown = False
        self._generation = 0
        self._snapshot = TrackingSnapshot.searching()
        self._vision_error: str | None = None
        self._inference_ms: float | None = None
        self._actual_detection_fps = 0.0
        self._last_completion_time: float | None = None

    def start(self) -> None:
        with self._condition:
            if self._thread is not None:
                return
            if self._shutdown:
                raise RuntimeError("vision service has been closed")
            self._thread = threading.Thread(
                target=self._worker,
                name="sentry-vision",
                daemon=False,
            )
            self._thread.start()
        self._logger.info("vision initialized; detection is disabled")

    def enable(self) -> bool:
        with self._state_lock:
            if self._shutdown:
                raise VisionInitializationError("vision service has been closed")
            if self._enabled:
                return False
            request_generation = self._generation
        self._ensure_detector()

        with self._condition:
            if self._shutdown:
                raise VisionInitializationError("vision service has been closed")
            if self._enabled:
                return False
            if self._generation != request_generation:
                return False
            self._enabled = True
            self._generation += 1
            self._snapshot = self._tracking.reset()
            self._vision_error = None
            self._inference_ms = None
            self._actual_detection_fps = 0.0
            self._last_completion_time = None
            self._condition.notify_all()
        self._logger.info("detection enabled for target class %s", self._target_class)
        return True

    def disable(self) -> bool:
        with self._condition:
            changed = self._enabled
            self._enabled = False
            self._generation += 1
            self._snapshot = self._tracking.reset()
            self._condition.notify_all()
        if changed:
            self._logger.info("detection disabled")
        return changed

    def close(self) -> None:
        with self._condition:
            if self._shutdown:
                return
            self._shutdown = True
            self._enabled = False
            self._generation += 1
            self._snapshot = self._tracking.reset()
            self._condition.notify_all()
        thread = self._thread
        if thread is not None and thread is not threading.current_thread():
            thread.join()

    def get_tracking_state(self) -> TrackingSnapshot:
        with self._state_lock:
            return self._snapshot

    def status(self) -> dict[str, Any]:
        with self._state_lock:
            snapshot = self._snapshot
            target = snapshot.target
            return {
                "vision_enabled": self._enabled,
                "vision_ready": self._detector is not None,
                "vision_model": (
                    None if self._model_path is None else str(self._model_path)
                ),
                "target_class": self._target_class,
                "confidence_threshold": self._confidence,
                "inference_size": self._inference_size,
                "detection_fps_limit": self._detection_fps_limit,
                "dead_zone_x": self._tracking.dead_zone_x,
                "dead_zone_y": self._tracking.dead_zone_y,
                "vision_error": self._vision_error,
                "tracking_state": snapshot.state.value,
                "target_detected": target is not None,
                "target_confidence": (
                    None if target is None else target.confidence
                ),
                "target_bbox": None if target is None else target.status()["bbox"],
                "target_center": (
                    None
                    if target is None
                    else {"x": target.center_x, "y": target.center_y}
                ),
                "pan_error": snapshot.pan_error,
                "tilt_error": snapshot.tilt_error,
                "pan_centered": snapshot.pan_centered,
                "tilt_centered": snapshot.tilt_centered,
                "locked": snapshot.locked,
                "detections": [
                    detection.status() for detection in snapshot.detections
                ],
                "vision_frame_width": snapshot.frame_width,
                "vision_frame_height": snapshot.frame_height,
                "inference_ms": self._inference_ms,
                "detection_fps": self._actual_detection_fps,
            }

    def _ensure_detector(self) -> None:
        with self._model_lock:
            with self._state_lock:
                if self._detector is not None:
                    return
                model_path = self._model_path
            if model_path is None:
                error = "no YOLO model configured; pass --model PATH"
                self._set_error(error)
                raise VisionConfigurationError(error)

            try:
                detector = self._detector_factory(
                    model_path,
                    self._confidence,
                    self._inference_size,
                )
                detector.validate_target_class(self._target_class)
            except DetectorConfigurationError as exc:
                self._set_error(str(exc))
                raise VisionConfigurationError(str(exc)) from exc
            except DetectorInitializationError as exc:
                self._set_error(str(exc))
                raise VisionInitializationError(str(exc)) from exc
            except Exception as exc:
                error = f"vision model initialization failed: {exc}"
                self._set_error(error)
                raise VisionInitializationError(error) from exc

            with self._state_lock:
                self._detector = detector
                self._vision_error = None
            self._logger.info("model loaded: %s", model_path)

    def _worker(self) -> None:
        period_s = 1.0 / self._detection_fps_limit
        while True:
            with self._condition:
                self._condition.wait_for(lambda: self._shutdown or self._enabled)
                if self._shutdown:
                    return
                generation = self._generation
                detector = self._detector
            if detector is None:
                self._fail_active_generation(
                    generation,
                    "vision detector is unavailable after enable",
                )
                continue

            cycle_started = time.monotonic()
            try:
                frame = self._camera_service.get_frame(copy=False)
                frame_height, frame_width = int(frame.shape[0]), int(frame.shape[1])
                inference_started = time.monotonic()
                detections = detector.detect(frame)
            except CameraUnavailableError as exc:
                self._fail_active_generation(generation, str(exc))
                continue
            except Exception as exc:
                self._fail_active_generation(generation, str(exc))
                continue

            completed = time.monotonic()
            inference_ms = (completed - inference_started) * 1000.0
            with self._condition:
                if (
                    self._shutdown
                    or not self._enabled
                    or generation != self._generation
                ):
                    continue

                target = self._selector.select(detections)
                previous_snapshot = self._snapshot
                self._snapshot = self._tracking.update(
                    detections,
                    target,
                    frame_width=frame_width,
                    frame_height=frame_height,
                )
                self._inference_ms = inference_ms
                self._update_detection_fps(completed)
                self._log_tracking_transition(previous_snapshot, self._snapshot)

                wait_s = max(0.0, period_s - (time.monotonic() - cycle_started))
                self._condition.wait_for(
                    lambda: (
                        self._shutdown
                        or not self._enabled
                        or generation != self._generation
                    ),
                    timeout=wait_s,
                )

    def _fail_active_generation(self, generation: int, error: str) -> None:
        with self._condition:
            if generation != self._generation or not self._enabled:
                return
            self._enabled = False
            self._generation += 1
            self._snapshot = self._tracking.reset()
            self._vision_error = error
            self._condition.notify_all()
        self._logger.error("vision error: %s", error)

    def _update_detection_fps(self, completed: float) -> None:
        if self._last_completion_time is not None:
            interval = completed - self._last_completion_time
            if interval > 0.0:
                instantaneous = 1.0 / interval
                if self._actual_detection_fps == 0.0:
                    self._actual_detection_fps = instantaneous
                else:
                    self._actual_detection_fps = (
                        0.8 * self._actual_detection_fps + 0.2 * instantaneous
                    )
        self._last_completion_time = completed

    def _log_tracking_transition(
        self,
        previous: TrackingSnapshot,
        current: TrackingSnapshot,
    ) -> None:
        if previous.target is None and current.target is not None:
            self._logger.info(
                "target acquired: %s confidence=%.3f",
                current.target.class_name,
                current.target.confidence,
            )
        elif previous.target is not None and current.target is None:
            self._logger.info("target lost")
        if previous.state != current.state:
            self._logger.info(
                "tracking state changed: %s -> %s",
                previous.state.value,
                current.state.value,
            )

    def _set_error(self, error: str) -> None:
        with self._state_lock:
            self._vision_error = error

    @staticmethod
    def _create_detector(
        model_path: Path,
        confidence: float,
        inference_size: int,
    ) -> Detector:
        return YoloDetector(
            model_path,
            confidence=confidence,
            inference_size=inference_size,
        )
