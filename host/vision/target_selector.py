"""Replaceable target-selection policy for detector results."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable

from .detector import Detection


@dataclass(frozen=True)
class TargetSelector:
    target_class: str
    confidence_threshold: float

    def __post_init__(self) -> None:
        if not self.target_class:
            raise ValueError("target class must not be empty")
        if not 0.0 <= self.confidence_threshold <= 1.0:
            raise ValueError("confidence threshold must be in the range 0..1")

    def select(self, detections: Iterable[Detection]) -> Detection | None:
        eligible = (
            detection
            for detection in detections
            if detection.class_name == self.target_class
            and detection.confidence >= self.confidence_threshold
        )
        return max(eligible, key=lambda detection: detection.confidence, default=None)
