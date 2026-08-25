import time

import cv2
import torch
from ultralytics import YOLO


# -----------------------------
# Configuration
# -----------------------------
MODEL_PATH = "yolo26n.pt"
CAMERA_INDEX = 0
CONFIDENCE_THRESHOLD = 0.1

# How close the target center must be to image center to count as centered
DEAD_ZONE_PIXELS = 40

TARGET_CLASS = "animal_mouse"  # Change this to the class you want to track

# -----------------------------
# Setup
# -----------------------------
device = 0 if torch.cuda.is_available() else "cpu"

print(f"Using device: {device}")
if torch.cuda.is_available():
    print(f"GPU: {torch.cuda.get_device_name(0)}")

model = YOLO(MODEL_PATH)

target_class_id = None

for class_id, class_name in model.names.items():
    if class_name == TARGET_CLASS:
        target_class_id = class_id
        break

if target_class_id is None:
    raise ValueError(f"Class '{TARGET_CLASS}' not found in model")

print(f"Target class: {TARGET_CLASS} ({target_class_id})")
cap = cv2.VideoCapture(CAMERA_INDEX)

if not cap.isOpened():
    raise RuntimeError(f"Could not open camera {CAMERA_INDEX}")

previous_time = time.time()

active_target_id = None

# -----------------------------
# Main loop
# -----------------------------
while True:
    success, frame = cap.read()

    if not success:
        print("Failed to read webcam frame")
        break

    height, width = frame.shape[:2]

    frame_center_x = width // 2
    frame_center_y = height // 2

    # Run YOLO
    results = model.track(
        frame,
        persist=True,
        tracker="bytetrack.yaml",
        conf=CONFIDENCE_THRESHOLD,
        classes=[target_class_id],
        device=device,
        verbose=False,
    )

    result = results[0]

    # Start with YOLO's annotated image
    display = result.plot()

    # -----------------------------
    # FPS
    # -----------------------------
    current_time = time.time()
    delta_time = current_time - previous_time
    previous_time = current_time

    fps = 1.0 / max(delta_time, 1e-6)

    cv2.putText(
        display,
        f"FPS: {fps:.1f}",
        (20, 30),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.7,
        (255, 255, 255),
        2,
    )

    # -----------------------------
    # Draw optical/image center
    # -----------------------------
    cv2.drawMarker(
        display,
        (frame_center_x, frame_center_y),
        (255, 255, 255),
        markerType=cv2.MARKER_CROSS,
        markerSize=30,
        thickness=2,
    )

    # -----------------------------
    # Read detections
    # -----------------------------
    detections = []

    if result.boxes is not None:
        for box in result.boxes:
            class_id = int(box.cls[0])
            confidence = float(box.conf[0])

            x1, y1, x2, y2 = box.xyxy[0].tolist()

            center_x = int((x1 + x2) / 2)
            center_y = int((y1 + y2) / 2)

            box_width = int(x2 - x1)
            box_height = int(y2 - y1)

            class_name = model.names[class_id]

            # Tracking ID
            if box.id is not None:
                track_id = int(box.id[0])
            else:
                track_id = None
                
            detection = {
                "class": class_name,
                "confidence": confidence,
                "center_x": center_x,
                "center_y": center_y,
                "width": box_width,
                "height": box_height,
                "track_id": track_id,
            }

            detections.append(detection)

    # -----------------------------
    # Pick target
    # Highest-confidence detection
    # -----------------------------
    if detections:
        target = None
        
            # If we already have a target, try to find it
        if active_target_id is not None:
            for detection in detections:
                if detection["track_id"] == active_target_id:
                    target = detection
                    active_target_id = None
                    break
                
        # If our old target disappeared, select a new one
        if target is None:
            target = max(
                detections,
                key=lambda detection: detection["confidence"]
            )

            active_target_id = target["track_id"]   

        target_x = target["center_x"]
        target_y = target["center_y"]

        error_x = target_x - frame_center_x
        error_y = target_y - frame_center_y

        # Draw target center
        cv2.circle(
            display,
            (target_x, target_y),
            8,
            (255, 255, 255),
            -1,
        )

        # Draw error vector
        cv2.line(
            display,
            (frame_center_x, frame_center_y),
            (target_x, target_y),
            (255, 255, 255),
            2,
        )

        # -----------------------------
        # Simulated controller
        # -----------------------------
        if error_x < -DEAD_ZONE_PIXELS:
            pan_command = "PAN LEFT"
        elif error_x > DEAD_ZONE_PIXELS:
            pan_command = "PAN RIGHT"
        else:
            pan_command = "PAN CENTERED"

        if error_y < -DEAD_ZONE_PIXELS:
            tilt_command = "TILT UP"
        elif error_y > DEAD_ZONE_PIXELS:
            tilt_command = "TILT DOWN"
        else:
            tilt_command = "TILT CENTERED"

        if (
            abs(error_x) <= DEAD_ZONE_PIXELS
            and abs(error_y) <= DEAD_ZONE_PIXELS
        ):
            state = "LOCKED"
        else:
            state = "TRACKING"

        # -----------------------------
        # Telemetry overlay
        # -----------------------------
        telemetry = [
            f"TARGET: {target['class']}",
            f"CONF: {target['confidence']:.2f}",
            f"CENTER: ({target_x}, {target_y})",
            f"ERROR: ({error_x}, {error_y})",
            pan_command,
            tilt_command,
            f"STATE: {state}",
        ]

        y = 60

        for line in telemetry:
            cv2.putText(
                display,
                line,
                (20, y),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.6,
                (255, 255, 255),
                2,
            )
            y += 25

    else:
        cv2.putText(
            display,
            "STATE: SEARCHING",
            (20, 60),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.7,
            (255, 255, 255),
            2,
        )

    # -----------------------------
    # Display
    # -----------------------------
    cv2.imshow("Sentry Object Detection", display)

    # Q = quit
    if cv2.waitKey(1) & 0xFF == ord("q"):
        break


# -----------------------------
# Cleanup
# -----------------------------
cap.release()
cv2.destroyAllWindows()