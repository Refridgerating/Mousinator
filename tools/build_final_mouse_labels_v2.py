#!/usr/bin/env python3
"""
Build and validate the canonical final annotation set for Sentry mouse images.

Reviewed images may validly contain:
- one or more mouse boxes
- zero boxes, if the human review determined no mouse is present

YOLO represents a negative image with an empty .txt label file.
"""

from __future__ import annotations

import argparse
import csv
import shutil
from collections import Counter
from pathlib import Path


IMAGE_EXTENSIONS = {
    ".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff"
}


def parse_args():
    p = argparse.ArgumentParser(
        description="Build final reviewed mouse annotation set."
    )
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
        "--corrected-labels",
        type=Path,
        default=Path("data/mouse_dataset/annotations/corrected/labels"),
    )
    p.add_argument(
        "--review-status",
        type=Path,
        default=Path(
            "data/mouse_dataset/annotations/review/review_status.csv"
        ),
    )
    p.add_argument(
        "--output",
        type=Path,
        default=Path("data/mouse_dataset/annotations/final"),
    )
    return p.parse_args()


def read_review_status(path: Path) -> dict[str, dict]:
    if not path.exists():
        raise SystemExit(f"Review status file not found: {path}")

    with path.open("r", newline="", encoding="utf-8") as f:
        return {
            row["filename"]: row
            for row in csv.DictReader(f)
        }


def validate_label_file(path: Path):
    """
    Returns:
        valid, num_boxes, errors

    An existing empty label file is valid and represents a YOLO negative image.
    """
    errors = []

    if not path.exists():
        return False, 0, ["missing_label_file"]

    text = path.read_text(encoding="utf-8").strip()

    # Empty YOLO label = valid negative image.
    if not text:
        return True, 0, []

    num_boxes = 0

    for line_number, line in enumerate(text.splitlines(), start=1):
        parts = line.split()

        if len(parts) != 5:
            errors.append(f"line_{line_number}:field_count")
            continue

        try:
            cls_id = int(parts[0])
            xc, yc, bw, bh = map(float, parts[1:])
        except ValueError:
            errors.append(f"line_{line_number}:parse_error")
            continue

        if cls_id != 0:
            errors.append(f"line_{line_number}:class_not_zero")

        if not (0.0 <= xc <= 1.0):
            errors.append(f"line_{line_number}:xc_out_of_range")

        if not (0.0 <= yc <= 1.0):
            errors.append(f"line_{line_number}:yc_out_of_range")

        if not (0.0 < bw <= 1.0):
            errors.append(f"line_{line_number}:width_out_of_range")

        if not (0.0 < bh <= 1.0):
            errors.append(f"line_{line_number}:height_out_of_range")

        x1 = xc - bw / 2
        x2 = xc + bw / 2
        y1 = yc - bh / 2
        y2 = yc + bh / 2

        tolerance = 1e-3

        if x1 < -tolerance or x2 > 1.0 + tolerance:
            errors.append(f"line_{line_number}:x_box_outside_image")

        if y1 < -tolerance or y2 > 1.0 + tolerance:
            errors.append(f"line_{line_number}:y_box_outside_image")

        num_boxes += 1

    return len(errors) == 0, num_boxes, errors


def main():
    args = parse_args()

    for required in [
        args.images,
        args.auto_labels,
        args.review_status,
    ]:
        if not required.exists():
            raise SystemExit(f"Required path not found: {required}")

    review = read_review_status(args.review_status)

    images = sorted(
        p for p in args.images.iterdir()
        if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS
    )

    if not images:
        raise SystemExit(f"No images found in: {args.images}")

    labels_out = args.output / "labels"

    if labels_out.exists():
        shutil.rmtree(labels_out)

    labels_out.mkdir(parents=True, exist_ok=True)

    report_rows = []
    counts = Counter()

    for image_path in images:
        filename = image_path.name
        stem = image_path.stem

        row = review.get(filename)

        if row is None:
            report_rows.append({
                "filename": filename,
                "decision": "",
                "label_source": "",
                "num_boxes": "",
                "status": "ERROR",
                "errors": "missing_review_decision",
            })
            counts["error"] += 1
            continue

        decision = row.get("decision", "").strip()

        if decision == "accepted":
            source = args.auto_labels / f"{stem}.txt"
            source_name = "auto"

        elif decision == "corrected":
            source = args.corrected_labels / f"{stem}.txt"
            source_name = "corrected"

        else:
            report_rows.append({
                "filename": filename,
                "decision": decision,
                "label_source": "",
                "num_boxes": "",
                "status": "ERROR",
                "errors": f"unresolved_decision:{decision or 'blank'}",
            })
            counts["error"] += 1
            continue

        valid, num_boxes, errors = validate_label_file(source)

        if not valid:
            report_rows.append({
                "filename": filename,
                "decision": decision,
                "label_source": source_name,
                "num_boxes": num_boxes,
                "status": "ERROR",
                "errors": "|".join(errors),
            })
            counts["error"] += 1
            continue

        destination = labels_out / f"{stem}.txt"
        shutil.copy2(source, destination)

        status = "OK_NEGATIVE" if num_boxes == 0 else "OK"

        report_rows.append({
            "filename": filename,
            "decision": decision,
            "label_source": source_name,
            "num_boxes": num_boxes,
            "status": status,
            "errors": "",
        })

        counts["ok"] += 1
        counts[source_name] += 1
        counts["boxes"] += num_boxes

        if num_boxes == 0:
            counts["negative_images"] += 1
        elif num_boxes > 1:
            counts["multi_box_images"] += 1

    image_names = {p.name for p in images}
    orphan_reviews = sorted(set(review) - image_names)

    report_path = args.output / "final_annotation_report.csv"

    fields = [
        "filename",
        "decision",
        "label_source",
        "num_boxes",
        "status",
        "errors",
    ]

    with report_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(report_rows)

    summary_lines = [
        "Sentry mouse final annotation set",
        "=" * 39,
        f"Reviewed images:          {len(images):,}",
        f"Final valid labels:       {counts['ok']:,}",
        f"  accepted auto labels:   {counts['auto']:,}",
        f"  corrected labels:       {counts['corrected']:,}",
        f"Positive images:          {counts['ok'] - counts['negative_images']:,}",
        f"Reviewed negative images: {counts['negative_images']:,}",
        f"Total mouse boxes:        {counts['boxes']:,}",
        f"Multi-mouse images:       {counts['multi_box_images']:,}",
        f"Errors / unresolved:      {counts['error']:,}",
        f"Orphan review rows:       {len(orphan_reviews):,}",
        "",
        f"Final labels: {labels_out}",
        f"Report:       {report_path}",
    ]

    if counts["error"]:
        summary_lines.extend([
            "",
            "ERRORS EXIST.",
            "Inspect rows with status=ERROR in final_annotation_report.csv",
            "before building the training dataset.",
        ])
    else:
        summary_lines.extend([
            "",
            "PASS: every reviewed image has a valid YOLO label.",
            "Empty label files are retained as valid negative examples.",
        ])

    summary = "\n".join(summary_lines)
    summary_path = args.output / "summary.txt"
    summary_path.write_text(summary, encoding="utf-8")

    print()
    print(summary)

    if counts["error"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
