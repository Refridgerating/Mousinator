"""Ultralytics YOLO adapter with no camera ownership."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Mapping

import numpy as np


class DetectorConfigurationError(ValueError):
    """Raised when model configuration cannot satisfy the vision request."""


class DetectorInitializationError(RuntimeError):
    """Raised when Ultralytics cannot initialize a configured model."""


@dataclass(frozen=True)
class Detection:
    class_id: int
    class_name: str
    confidence: float
    x1: float
    y1: float
    x2: float
    y2: float
    tracking_id: int | None = None

    @property
    def center_x(self) -> float:
        return (self.x1 + self.x2) / 2.0

    @property
    def center_y(self) -> float:
        return (self.y1 + self.y2) / 2.0

    def status(self) -> dict[str, Any]:
        return {
            "class_id": self.class_id,
            "class_name": self.class_name,
            "confidence": self.confidence,
            "bbox": {
                "x1": self.x1,
                "y1": self.y1,
                "x2": self.x2,
                "y2": self.y2,
            },
            "center": {"x": self.center_x, "y": self.center_y},
            "tracking_id": self.tracking_id,
        }


class YoloDetector:
    """Load one Ultralytics model and return camera-coordinate detections."""

    def __init__(
        self,
        model_path: Path,
        *,
        confidence: float,
        inference_size: int,
        model_loader: Callable[[str], Any] | None = None,
    ) -> None:
        if not 0.0 <= confidence <= 1.0:
            raise DetectorConfigurationError("confidence must be in the range 0..1")
        if inference_size <= 0:
            raise DetectorConfigurationError("inference size must be positive")

        path = Path(model_path).expanduser()
        if not path.exists():
            raise DetectorConfigurationError(f"YOLO model does not exist: {path}")

        loader = model_loader or self._default_model_loader
        try:
            self._model = loader(str(path))
        except DetectorConfigurationError:
            raise
        except Exception as exc:
            raise DetectorInitializationError(
                f"failed to load YOLO model {path}: {exc}"
            ) from exc

        self.model_path = path.resolve()
        self.confidence = confidence
        self.inference_size = inference_size
        self.class_names = self._normalize_names(getattr(self._model, "names", None))
        if not self.class_names:
            raise DetectorInitializationError("YOLO model exposes no class-name mapping")

    def validate_target_class(self, target_class: str) -> None:
        if target_class not in self.class_names.values():
            available = ", ".join(sorted(self.class_names.values()))
            raise DetectorConfigurationError(
                f"target class {target_class!r} is not present in the model; "
                f"available classes: {available}"
            )

    def detect(self, rgb_frame: Any) -> tuple[Detection, ...]:
        frame = np.asarray(rgb_frame)
        if frame.ndim != 3 or frame.shape[2] != 3:
            raise ValueError("YOLO input must be an RGB image with three channels")
        source_height, source_width = int(frame.shape[0]), int(frame.shape[1])
        if source_width <= 0 or source_height <= 0:
            raise ValueError("YOLO input dimensions must be positive")

        inference_frame, scale_x, scale_y = self._prepare_frame(frame)
        try:
            results = self._model.predict(
                source=inference_frame,
                conf=self.confidence,
                imgsz=self.inference_size,
                verbose=False,
            )
        except Exception as exc:
            raise RuntimeError(f"YOLO inference failed: {exc}") from exc

        if not results:
            return ()
        boxes = getattr(results[0], "boxes", None)
        if boxes is None:
            return ()

        coordinates = self._to_numpy(getattr(boxes, "xyxy", ()))
        confidences = self._to_numpy(getattr(boxes, "conf", ())).reshape(-1)
        class_ids = self._to_numpy(getattr(boxes, "cls", ())).reshape(-1)
        tracking_ids_value = getattr(boxes, "id", None)
        tracking_ids = (
            None
            if tracking_ids_value is None
            else self._to_numpy(tracking_ids_value).reshape(-1)
        )

        detections = []
        for index, raw_coordinates in enumerate(coordinates):
            confidence = float(confidences[index])
            if confidence < self.confidence:
                continue
            class_id = int(class_ids[index])
            class_name = self.class_names.get(class_id)
            if class_name is None:
                raise RuntimeError(f"YOLO returned unknown class id {class_id}")
            x1, y1, x2, y2 = (float(value) for value in raw_coordinates[:4])
            detections.append(
                Detection(
                    class_id=class_id,
                    class_name=class_name,
                    confidence=confidence,
                    x1=self._clamp(x1 * scale_x, 0.0, float(source_width)),
                    y1=self._clamp(y1 * scale_y, 0.0, float(source_height)),
                    x2=self._clamp(x2 * scale_x, 0.0, float(source_width)),
                    y2=self._clamp(y2 * scale_y, 0.0, float(source_height)),
                    tracking_id=(
                        None if tracking_ids is None else int(tracking_ids[index])
                    ),
                )
            )
        return tuple(detections)

    def _prepare_frame(self, rgb_frame: np.ndarray) -> tuple[np.ndarray, float, float]:
        try:
            import cv2
        except ImportError as exc:  # pragma: no cover - Ultralytics dependency
            raise DetectorInitializationError(
                "OpenCV is required by the Ultralytics vision adapter"
            ) from exc

        source_height, source_width = rgb_frame.shape[:2]
        largest_dimension = max(source_width, source_height)
        if largest_dimension > self.inference_size:
            resize_scale = self.inference_size / float(largest_dimension)
            inference_width = max(1, round(source_width * resize_scale))
            inference_height = max(1, round(source_height * resize_scale))
            resized_rgb = cv2.resize(
                rgb_frame,
                (inference_width, inference_height),
                interpolation=cv2.INTER_AREA,
            )
        else:
            resized_rgb = rgb_frame
            inference_height, inference_width = source_height, source_width

        # CameraService publishes RGB. Ultralytics' ndarray interface expects BGR.
        inference_bgr = cv2.cvtColor(resized_rgb, cv2.COLOR_RGB2BGR)
        return (
            inference_bgr,
            source_width / float(inference_width),
            source_height / float(inference_height),
        )

    @staticmethod
    def _default_model_loader(model_path: str) -> Any:
        try:
            from ultralytics import YOLO
        except ImportError as exc:  # pragma: no cover - optional Pi dependency
            raise DetectorInitializationError(
                "Ultralytics is unavailable; install it with "
                "python -m pip install ultralytics"
            ) from exc
        return YOLO(model_path)

    @staticmethod
    def _normalize_names(raw_names: Any) -> dict[int, str]:
        if isinstance(raw_names, Mapping):
            return {int(class_id): str(name) for class_id, name in raw_names.items()}
        if isinstance(raw_names, (list, tuple)):
            return {class_id: str(name) for class_id, name in enumerate(raw_names)}
        return {}

    @staticmethod
    def _to_numpy(value: Any) -> np.ndarray:
        if hasattr(value, "detach"):
            value = value.detach()
        if hasattr(value, "cpu"):
            value = value.cpu()
        if hasattr(value, "numpy"):
            value = value.numpy()
        return np.asarray(value)

    @staticmethod
    def _clamp(value: float, minimum: float, maximum: float) -> float:
        return min(maximum, max(minimum, value))
