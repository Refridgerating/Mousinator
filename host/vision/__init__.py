"""Optional YOLO detection and camera-coordinate tracking state."""

from .detector import Detection, YoloDetector
from .target_selector import TargetSelector
from .tracking_state import TrackingSnapshot, TrackingState, TrackingStateMachine
from .vision_service import (
    VisionConfigurationError,
    VisionInitializationError,
    VisionService,
)

__all__ = [
    "Detection",
    "TargetSelector",
    "TrackingSnapshot",
    "TrackingState",
    "TrackingStateMachine",
    "VisionConfigurationError",
    "VisionInitializationError",
    "VisionService",
    "YoloDetector",
]
