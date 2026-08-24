#!/usr/bin/env python3
"""
Interactive box-correction tool for Sentry mouse annotations.

Queue:
    Only images marked decision=needs_correction in:
    data/mouse_dataset/annotations/review/review_status.csv

Source labels:
    data/mouse_dataset/annotations/auto/labels

Corrected labels:
    data/mouse_dataset/annotations/corrected/labels

Controls:
    LEFT-DRAG   Add a box
    RIGHT-CLICK Remove the box under the cursor
    C           Clear all boxes
    R           Restore original auto-label proposal
    Z           Undo last box edit
    SPACE       Save corrected labels and go to next image
    A           Previous image
    Q           Save review state and quit

Original auto-label files are never modified.
"""

from __future__ import annotations

import argparse
import csv
from copy import deepcopy
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageOps


IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff"}


def parse_args():
    p = argparse.ArgumentParser(description="Correct Sentry mouse bounding boxes.")
    p.add_argument(
        "--images",
        type=Path,
        default=Path("data/mouse_dataset/clean/positives"),
    )
    p.add_argument(
        "--auto-labels",
        type=Path,
        default=Path("data/mouse_dataset/annotations/auto/labels"),
    )
    p.add_argument(
        "--review-status",
        type=Path,
        default=Path("data/mouse_dataset/annotations/review/review_status.csv"),
    )
    p.add_argument(
        "--corrected-labels",
        type=Path,
        default=Path("data/mouse_dataset/annotations/corrected/labels"),
    )
    return p.parse_args()


def read_csv(path: Path) -> list[dict]:
    if not path.exists():
        raise SystemExit(f"Required file not found: {path}")
    with path.open("r", newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def write_review_status(path: Path, rows: list[dict]):
    if not rows:
        return

    fieldnames = list(rows[0].keys())

    temp = path.with_suffix(".tmp")
    with temp.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    temp.replace(path)


def load_image(path: Path) -> np.ndarray:
    with Image.open(path) as src:
        image = ImageOps.exif_transpose(src).convert("RGB")
    arr = np.asarray(image)
    return cv2.cvtColor(arr, cv2.COLOR_RGB2BGR)


def parse_yolo(path: Path, width: int, height: int):
    boxes = []
    if not path.exists():
        return boxes

    text = path.read_text(encoding="utf-8").strip()
    if not text:
        return boxes

    for line in text.splitlines():
        parts = line.split()
        if len(parts) != 5:
            continue

        cls_id = int(parts[0])
        xc, yc, bw, bh = map(float, parts[1:])

        x1 = int(round((xc - bw / 2.0) * width))
        y1 = int(round((yc - bh / 2.0) * height))
        x2 = int(round((xc + bw / 2.0) * width))
        y2 = int(round((yc + bh / 2.0) * height))

        x1 = max(0, min(width - 1, x1))
        y1 = max(0, min(height - 1, y1))
        x2 = max(0, min(width - 1, x2))
        y2 = max(0, min(height - 1, y2))

        if x2 > x1 and y2 > y1:
            boxes.append([x1, y1, x2, y2, cls_id])

    return boxes


def save_yolo(path: Path, boxes, width: int, height: int):
    lines = []

    for x1, y1, x2, y2, cls_id in boxes:
        x1 = max(0, min(width, x1))
        y1 = max(0, min(height, y1))
        x2 = max(0, min(width, x2))
        y2 = max(0, min(height, y2))

        if x2 <= x1 or y2 <= y1:
            continue

        xc = ((x1 + x2) / 2.0) / width
        yc = ((y1 + y2) / 2.0) / height
        bw = (x2 - x1) / width
        bh = (y2 - y1) / height

        lines.append(
            f"{cls_id} {xc:.6f} {yc:.6f} {bw:.6f} {bh:.6f}"
        )

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "\n".join(lines) + ("\n" if lines else ""),
        encoding="utf-8",
    )


def point_in_box(x, y, box):
    x1, y1, x2, y2, _ = box
    return x1 <= x <= x2 and y1 <= y <= y2


def fit_scale(width: int, height: int, max_width=1500, max_height=820):
    return min(max_width / width, max_height / height, 1.0)


class Editor:
    def __init__(self):
        self.original = None
        self.boxes = []
        self.auto_boxes = []
        self.undo_stack = []

        self.scale = 1.0
        self.header = 105

        self.dragging = False
        self.drag_start = None
        self.drag_current = None

    def snapshot(self):
        self.undo_stack.append(deepcopy(self.boxes))
        if len(self.undo_stack) > 50:
            self.undo_stack.pop(0)

    def undo(self):
        if self.undo_stack:
            self.boxes = self.undo_stack.pop()

    def display_to_image(self, x, y):
        iy = y - self.header
        ix = x

        if iy < 0:
            return None

        px = int(round(ix / self.scale))
        py = int(round(iy / self.scale))

        h, w = self.original.shape[:2]
        px = max(0, min(w - 1, px))
        py = max(0, min(h - 1, py))

        return px, py

    def mouse_callback(self, event, x, y, flags, param):
        point = self.display_to_image(x, y)

        if event == cv2.EVENT_LBUTTONDOWN:
            if point is None:
                return
            self.dragging = True
            self.drag_start = point
            self.drag_current = point

        elif event == cv2.EVENT_MOUSEMOVE and self.dragging:
            if point is not None:
                self.drag_current = point

        elif event == cv2.EVENT_LBUTTONUP and self.dragging:
            self.dragging = False

            if point is None or self.drag_start is None:
                self.drag_start = None
                self.drag_current = None
                return

            x1, y1 = self.drag_start
            x2, y2 = point

            x1, x2 = sorted((x1, x2))
            y1, y2 = sorted((y1, y2))

            self.drag_start = None
            self.drag_current = None

            if x2 - x1 >= 4 and y2 - y1 >= 4:
                self.snapshot()
                self.boxes.append([x1, y1, x2, y2, 0])

        elif event == cv2.EVENT_RBUTTONDOWN:
            if point is None:
                return

            px, py = point
            matches = [
                (i, (b[2] - b[0]) * (b[3] - b[1]))
                for i, b in enumerate(self.boxes)
                if point_in_box(px, py, b)
            ]

            if matches:
                # If boxes overlap, remove the smallest box under cursor.
                index = min(matches, key=lambda t: t[1])[0]
                self.snapshot()
                self.boxes.pop(index)

    def render(self, filename, index, total):
        image = self.original.copy()

        for i, (x1, y1, x2, y2, cls_id) in enumerate(self.boxes, start=1):
            cv2.rectangle(image, (x1, y1), (x2, y2), (0, 255, 0), 3)
            cv2.putText(
                image,
                str(i),
                (x1 + 4, max(18, y1 + 20)),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.7,
                (0, 255, 0),
                2,
                cv2.LINE_AA,
            )

        if self.dragging and self.drag_start and self.drag_current:
            x1, y1 = self.drag_start
            x2, y2 = self.drag_current
            cv2.rectangle(image, (x1, y1), (x2, y2), (0, 255, 255), 2)

        h, w = image.shape[:2]
        self.scale = fit_scale(w, h)

        if self.scale < 1.0:
            image = cv2.resize(
                image,
                (int(round(w * self.scale)), int(round(h * self.scale))),
                interpolation=cv2.INTER_AREA,
            )

        display_h, display_w = image.shape[:2]

        canvas = np.zeros(
            (display_h + self.header, display_w, 3),
            dtype=np.uint8,
        )
        canvas[:] = (25, 25, 25)
        canvas[self.header:, :] = image

        cv2.putText(
            canvas,
            f"{index + 1}/{total}   boxes={len(self.boxes)}   NEEDS CORRECTION",
            (15, 27),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.68,
            (255, 255, 255),
            2,
            cv2.LINE_AA,
        )
        cv2.putText(
            canvas,
            filename,
            (15, 56),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.52,
            (220, 220, 220),
            1,
            cv2.LINE_AA,
        )
        cv2.putText(
            canvas,
            "Drag:add  Right-click:remove  C:clear  R:reset  Z:undo",
            (15, 82),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.48,
            (205, 205, 205),
            1,
            cv2.LINE_AA,
        )
        cv2.putText(
            canvas,
            "SPACE:save+next   A:back   Q:quit",
            (15, 101),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.48,
            (205, 205, 205),
            1,
            cv2.LINE_AA,
        )

        return canvas


def main():
    args = parse_args()

    review_rows = read_csv(args.review_status)

    queue_rows = [
        row for row in review_rows
        if row.get("decision", "").strip() == "needs_correction"
    ]

    if not queue_rows:
        print("No images are marked needs_correction.")
        return

    image_index = {
        p.name: p
        for p in args.images.iterdir()
        if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS
    }

    # Skip rows already corrected in a prior session.
    pending = []
    for row in queue_rows:
        filename = row["filename"]
        corrected_path = args.corrected_labels / f"{Path(filename).stem}.txt"

        if row.get("decision", "").strip() == "corrected":
            continue

        if filename in image_index:
            pending.append(row)

    if not pending:
        print("No pending correction images found.")
        return

    args.corrected_labels.mkdir(parents=True, exist_ok=True)

    row_by_filename = {row["filename"]: row for row in review_rows}

    editor = Editor()
    window = "Sentry Box Correction"
    cv2.namedWindow(window, cv2.WINDOW_NORMAL)
    cv2.setMouseCallback(window, editor.mouse_callback)

    index = 0

    while 0 <= index < len(pending):
        row = pending[index]
        filename = row["filename"]
        image_path = image_index[filename]
        auto_label_path = args.auto_labels / f"{Path(filename).stem}.txt"
        corrected_path = args.corrected_labels / f"{Path(filename).stem}.txt"

        editor.original = load_image(image_path)
        h, w = editor.original.shape[:2]

        editor.auto_boxes = parse_yolo(auto_label_path, w, h)

        # If a partially corrected label already exists, resume from it.
        if corrected_path.exists():
            editor.boxes = parse_yolo(corrected_path, w, h)
        else:
            editor.boxes = deepcopy(editor.auto_boxes)

        editor.undo_stack = []
        editor.dragging = False
        editor.drag_start = None
        editor.drag_current = None

        while True:
            display = editor.render(filename, index, len(pending))
            cv2.imshow(window, display)
            key = cv2.waitKey(20) & 0xFF

            if key == 255:
                continue

            if key == ord("q"):
                write_review_status(args.review_status, review_rows)
                cv2.destroyAllWindows()
                print("Quit. Previously saved corrections are preserved.")
                return

            if key == ord("a"):
                index = max(0, index - 1)
                break

            if key == ord("c"):
                editor.snapshot()
                editor.boxes = []
                continue

            if key == ord("r"):
                editor.snapshot()
                editor.boxes = deepcopy(editor.auto_boxes)
                continue

            if key == ord("z"):
                editor.undo()
                continue

            if key == ord(" "):
                save_yolo(corrected_path, editor.boxes, w, h)

                target_row = row_by_filename[filename]
                target_row["decision"] = "corrected"
                target_row["reviewed"] = "yes"

                write_review_status(args.review_status, review_rows)

                print(
                    f"corrected   boxes={len(editor.boxes):2d}   {filename}"
                )

                index += 1
                break

    cv2.destroyAllWindows()
    write_review_status(args.review_status, review_rows)

    print()
    print("Correction queue complete.")
    print(f"Corrected labels: {args.corrected_labels}")
    print(f"Review status:    {args.review_status}")


if __name__ == "__main__":
    main()
