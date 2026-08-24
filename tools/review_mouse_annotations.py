#!/usr/bin/env python3
"""
Fast QA reviewer for Sentry mouse auto-annotations.

Controls
--------
SPACE : accept current annotation and advance
X     : mark "needs_correction" and advance
A     : go back one image
Q     : save and quit

Priority order
--------------
1. NO_BOX
2. MULTI_BOX (2+ boxes)
3. LOW_CONFIDENCE
4. LARGE_BOX
5. SMALL_BOX
6. NORMAL

Inputs
------
data/mouse_dataset/clean/positives
data/mouse_dataset/annotations/auto/labels
data/mouse_dataset/annotations/auto/annotation_manifest.csv

Outputs
-------
data/mouse_dataset/annotations/review/review_status.csv

The script is resumable. Decisions are written after every accept/reject.
Nothing is deleted or modified in the source images or auto-label files.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass, asdict
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageOps


IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff"}


@dataclass
class ReviewItem:
    filename: str
    label_file: str
    num_boxes: int
    best_score: float | None
    max_box_area: float
    min_box_area: float
    priority: str
    priority_rank: int
    decision: str = ""
    reviewed: str = ""


def parse_args():
    p = argparse.ArgumentParser(description="Review Sentry mouse auto-annotations.")
    p.add_argument(
        "--images",
        type=Path,
        default=Path("data/mouse_dataset/clean/positives"),
    )
    p.add_argument(
        "--labels",
        type=Path,
        default=Path("data/mouse_dataset/annotations/auto/labels"),
    )
    p.add_argument(
        "--manifest",
        type=Path,
        default=Path("data/mouse_dataset/annotations/auto/annotation_manifest.csv"),
    )
    p.add_argument(
        "--review-dir",
        type=Path,
        default=Path("data/mouse_dataset/annotations/review"),
    )
    p.add_argument(
        "--low-confidence",
        type=float,
        default=0.25,
        help="Priority threshold for low-confidence detections.",
    )
    p.add_argument(
        "--large-box",
        type=float,
        default=0.65,
        help="Flag if any box occupies more than this fraction of the image.",
    )
    p.add_argument(
        "--small-box",
        type=float,
        default=0.01,
        help="Flag if any box occupies less than this fraction of the image.",
    )
    p.add_argument(
        "--flagged-only",
        action="store_true",
        help="Review only prioritized/flagged images, skipping NORMAL.",
    )
    p.add_argument(
        "--include-reviewed",
        action="store_true",
        help="Include images that already have a saved decision.",
    )
    return p.parse_args()


def load_manifest(path: Path) -> dict[str, dict]:
    if not path.exists():
        raise SystemExit(
            f"Annotation manifest not found: {path}\n"
            "Wait for auto_label_mouse.py to finish, then run this reviewer."
        )

    result = {}
    with path.open("r", newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            result[row["filename"]] = row
    return result


def load_existing_reviews(path: Path) -> dict[str, dict]:
    if not path.exists():
        return {}

    result = {}
    with path.open("r", newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            result[row["filename"]] = row
    return result


def parse_yolo_label(path: Path) -> list[tuple[int, float, float, float, float]]:
    if not path.exists():
        return []

    boxes = []
    text = path.read_text(encoding="utf-8").strip()
    if not text:
        return boxes

    for line_number, line in enumerate(text.splitlines(), start=1):
        parts = line.split()
        if len(parts) != 5:
            raise ValueError(f"{path}:{line_number}: expected 5 fields")

        cls_id = int(parts[0])
        xc, yc, w, h = map(float, parts[1:])

        # Clamp only for visualization robustness. The QA should still catch bad boxes.
        boxes.append((cls_id, xc, yc, w, h))

    return boxes


def classify_priority(
    num_boxes: int,
    best_score: float | None,
    areas: list[float],
    low_confidence: float,
    large_box: float,
    small_box: float,
) -> tuple[str, int]:
    if num_boxes == 0:
        return "NO_BOX", 0

    if num_boxes >= 2:
        return "MULTI_BOX", 1

    if best_score is not None and best_score < low_confidence:
        return "LOW_CONFIDENCE", 2

    if areas and max(areas) > large_box:
        return "LARGE_BOX", 3

    if areas and min(areas) < small_box:
        return "SMALL_BOX", 4

    return "NORMAL", 5


def build_queue(args, manifest, existing_reviews):
    image_paths = {
        p.name: p
        for p in args.images.iterdir()
        if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS
    }

    items = []

    for filename, manifest_row in manifest.items():
        if filename not in image_paths:
            continue

        label_name = manifest_row.get("label_file") or f"{Path(filename).stem}.txt"
        label_path = args.labels / label_name
        boxes = parse_yolo_label(label_path)
        areas = [max(0.0, b[3]) * max(0.0, b[4]) for b in boxes]

        score_text = (manifest_row.get("best_score") or "").strip()
        try:
            best_score = float(score_text) if score_text else None
        except ValueError:
            best_score = None

        num_boxes = len(boxes)

        priority, rank = classify_priority(
            num_boxes,
            best_score,
            areas,
            args.low_confidence,
            args.large_box,
            args.small_box,
        )

        if args.flagged_only and priority == "NORMAL":
            continue

        existing = existing_reviews.get(filename, {})
        decision = existing.get("decision", "")
        reviewed = existing.get("reviewed", "")

        if decision and not args.include_reviewed:
            continue

        items.append(
            ReviewItem(
                filename=filename,
                label_file=label_name,
                num_boxes=num_boxes,
                best_score=best_score,
                max_box_area=max(areas) if areas else 0.0,
                min_box_area=min(areas) if areas else 0.0,
                priority=priority,
                priority_rank=rank,
                decision=decision,
                reviewed=reviewed,
            )
        )

    def sort_key(item: ReviewItem):
        # Within low-confidence, worst confidence first.
        conf = item.best_score if item.best_score is not None else -1.0

        if item.priority == "LOW_CONFIDENCE":
            secondary = conf
        elif item.priority == "LARGE_BOX":
            secondary = -item.max_box_area
        elif item.priority == "SMALL_BOX":
            secondary = item.min_box_area
        elif item.priority == "MULTI_BOX":
            secondary = -item.num_boxes
        else:
            secondary = 0.0

        return (item.priority_rank, secondary, item.filename)

    items.sort(key=sort_key)
    return items, image_paths


def save_reviews(path: Path, all_reviews: dict[str, dict]):
    path.parent.mkdir(parents=True, exist_ok=True)

    fields = [
        "filename",
        "label_file",
        "decision",
        "reviewed",
        "priority",
        "num_boxes",
        "best_score",
        "max_box_area",
        "min_box_area",
    ]

    rows = sorted(all_reviews.values(), key=lambda r: r["filename"])

    temp = path.with_suffix(".tmp")
    with temp.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

    temp.replace(path)


def image_to_bgr(path: Path):
    with Image.open(path) as src:
        image = ImageOps.exif_transpose(src).convert("RGB")
    arr = np.asarray(image)
    return cv2.cvtColor(arr, cv2.COLOR_RGB2BGR)


def fit_to_screen(img, max_width=1500, max_height=900):
    h, w = img.shape[:2]
    scale = min(max_width / w, max_height / h, 1.0)

    if scale < 1.0:
        img = cv2.resize(
            img,
            (int(w * scale), int(h * scale)),
            interpolation=cv2.INTER_AREA,
        )

    return img


def draw_boxes(img, boxes):
    h, w = img.shape[:2]

    for cls_id, xc, yc, bw, bh in boxes:
        x1 = int((xc - bw / 2) * w)
        y1 = int((yc - bh / 2) * h)
        x2 = int((xc + bw / 2) * w)
        y2 = int((yc + bh / 2) * h)

        x1 = max(0, min(w - 1, x1))
        y1 = max(0, min(h - 1, y1))
        x2 = max(0, min(w - 1, x2))
        y2 = max(0, min(h - 1, y2))

        cv2.rectangle(img, (x1, y1), (x2, y2), (0, 0, 255), 3)

    return img


def add_header(img, item: ReviewItem, index: int, total: int):
    header_height = 105
    h, w = img.shape[:2]
    canvas = np.zeros((h + header_height, w, 3), dtype=np.uint8)
    canvas[:] = (25, 25, 25)
    canvas[header_height:, :] = img

    score = "-" if item.best_score is None else f"{item.best_score:.3f}"
    line1 = (
        f"{index + 1}/{total}  {item.priority}  "
        f"boxes={item.num_boxes}  score={score}"
    )
    line2 = item.filename
    line3 = "SPACE accept   X needs correction   A back   Q quit"

    cv2.putText(
        canvas, line1, (15, 28),
        cv2.FONT_HERSHEY_SIMPLEX, 0.70, (255, 255, 255), 2, cv2.LINE_AA
    )
    cv2.putText(
        canvas, line2, (15, 58),
        cv2.FONT_HERSHEY_SIMPLEX, 0.55, (220, 220, 220), 1, cv2.LINE_AA
    )
    cv2.putText(
        canvas, line3, (15, 88),
        cv2.FONT_HERSHEY_SIMPLEX, 0.55, (200, 200, 200), 1, cv2.LINE_AA
    )

    return canvas


def main():
    args = parse_args()

    for required in [args.images, args.labels]:
        if not required.exists():
            raise SystemExit(f"Required directory not found: {required}")

    review_path = args.review_dir / "review_status.csv"
    manifest = load_manifest(args.manifest)
    existing_reviews = load_existing_reviews(review_path)

    queue, image_paths = build_queue(args, manifest, existing_reviews)

    if not queue:
        print("Nothing to review.")
        return

    # Keep all prior rows and update them as decisions are made.
    all_reviews = dict(existing_reviews)

    priority_counts = {}
    for item in queue:
        priority_counts[item.priority] = priority_counts.get(item.priority, 0) + 1

    print("Review queue")
    print("============")
    for priority in [
        "NO_BOX",
        "MULTI_BOX",
        "LOW_CONFIDENCE",
        "LARGE_BOX",
        "SMALL_BOX",
        "NORMAL",
    ]:
        if priority in priority_counts:
            print(f"{priority:16s} {priority_counts[priority]:,}")
    print(f"{'TOTAL':16s} {len(queue):,}")
    print()
    print("SPACE accept | X needs correction | A back | Q quit")
    print()

    index = 0
    window = "Sentry Annotation Review"
    cv2.namedWindow(window, cv2.WINDOW_NORMAL)

    while 0 <= index < len(queue):
        item = queue[index]
        image_path = image_paths[item.filename]
        label_path = args.labels / item.label_file

        try:
            img = image_to_bgr(image_path)
            boxes = parse_yolo_label(label_path)
            img = draw_boxes(img, boxes)
            img = fit_to_screen(img)
            display = add_header(img, item, index, len(queue))
        except Exception as exc:
            print(f"ERROR loading {item.filename}: {exc}")
            index += 1
            continue

        cv2.imshow(window, display)
        key = cv2.waitKey(0) & 0xFF

        if key == ord("q"):
            break

        if key == ord("a"):
            index = max(0, index - 1)
            continue

        if key == ord(" "):
            decision = "accepted"
        elif key == ord("x"):
            decision = "needs_correction"
        else:
            continue

        row = {
            "filename": item.filename,
            "label_file": item.label_file,
            "decision": decision,
            "reviewed": "yes",
            "priority": item.priority,
            "num_boxes": item.num_boxes,
            "best_score": (
                "" if item.best_score is None else f"{item.best_score:.6f}"
            ),
            "max_box_area": f"{item.max_box_area:.6f}",
            "min_box_area": f"{item.min_box_area:.6f}",
        }

        all_reviews[item.filename] = row
        save_reviews(review_path, all_reviews)

        print(f"{decision:16s} {item.priority:16s} {item.filename}")
        index += 1

    cv2.destroyAllWindows()
    save_reviews(review_path, all_reviews)

    accepted = sum(
        1 for row in all_reviews.values()
        if row.get("decision") == "accepted"
    )
    needs_correction = sum(
        1 for row in all_reviews.values()
        if row.get("decision") == "needs_correction"
    )

    print()
    print("Review saved")
    print("============")
    print(f"Accepted:          {accepted:,}")
    print(f"Needs correction:  {needs_correction:,}")
    print(f"Status file:        {review_path}")


if __name__ == "__main__":
    main()
