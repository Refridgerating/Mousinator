"""Camera-coordinate tracking state independent of detection and motion."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Iterable

from .detector import Detection


class TrackingState(str, Enum):
    SEARCHING = "SEARCHING"
    DETECTED = "DETECTED"
    TRACKING = "TRACKING"
    LOCKED = "LOCKED"


@dataclass(frozen=True)
class TrackingSnapshot:
    state: TrackingState
    detections: tuple[Detection, ...]
    target: Detection | None
    frame_width: int | None
    frame_height: int | None
    pan_error: float | None
    tilt_error: float | None
    pan_centered: bool
    tilt_centered: bool
    locked: bool

    @classmethod
    def searching(cls) -> "TrackingSnapshot":
        return cls(
            state=TrackingState.SEARCHING,
            detections=(),
            target=None,
            frame_width=None,
            frame_height=None,
            pan_error=None,
            tilt_error=None,
            pan_centered=False,
            tilt_centered=False,
            locked=False,
        )


class TrackingStateMachine:
    def __init__(self, *, dead_zone_x: float, dead_zone_y: float) -> None:
        for value, name in ((dead_zone_x, "x"), (dead_zone_y, "y")):
            if not 0.0 <= value <= 1.0:
                raise ValueError(f"dead-zone {name} must be in the range 0..1")
        self.dead_zone_x = dead_zone_x
        self.dead_zone_y = dead_zone_y
        self._target_was_present = False

    def reset(self) -> TrackingSnapshot:
        self._target_was_present = False
        return TrackingSnapshot.searching()

    def update(
        self,
        detections: Iterable[Detection],
        target: Detection | None,
        *,
        frame_width: int,
        frame_height: int,
    ) -> TrackingSnapshot:
        if frame_width <= 0 or frame_height <= 0:
            raise ValueError("tracking frame dimensions must be positive")
        detection_tuple = tuple(detections)
        if target is None:
            self._target_was_present = False
            return TrackingSnapshot(
                state=TrackingState.SEARCHING,
                detections=(),
                target=None,
                frame_width=frame_width,
                frame_height=frame_height,
                pan_error=None,
                tilt_error=None,
                pan_centered=False,
                tilt_centered=False,
                locked=False,
            )

        image_center_x = frame_width / 2.0
        image_center_y = frame_height / 2.0
        pan_error = (target.center_x - image_center_x) / image_center_x
        tilt_error = (target.center_y - image_center_y) / image_center_y
        pan_centered = abs(pan_error) <= self.dead_zone_x
        tilt_centered = abs(tilt_error) <= self.dead_zone_y
        locked = pan_centered and tilt_centered

        if not self._target_was_present:
            state = TrackingState.DETECTED
        elif locked:
            state = TrackingState.LOCKED
        else:
            state = TrackingState.TRACKING
        self._target_was_present = True

        return TrackingSnapshot(
            state=state,
            detections=detection_tuple,
            target=target,
            frame_width=frame_width,
            frame_height=frame_height,
            pan_error=pan_error,
            tilt_error=tilt_error,
            pan_centered=pan_centered,
            tilt_centered=tilt_centered,
            locked=locked,
        )
