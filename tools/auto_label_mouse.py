#!/usr/bin/env python3
"""
Auto-annotate Sentry mouse-positive images with Grounding DINO.

Input:
    data/mouse_dataset/clean/positives

Outputs:
    data/mouse_dataset/annotations/auto/labels/*.txt
    data/mouse_dataset/annotations/auto/previews/*
    data/mouse_dataset/annotations/auto/annotation_manifest.csv

The generated labels are YOLO detection format:
    class_id x_center y_center width height

Class:
    0 = mouse

IMPORTANT:
These are annotation proposals, not final training labels. Review them before
building the train/val/test dataset.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import torch
from PIL import Image, ImageDraw, ImageFont, ImageOps
from transformers import AutoModelForZeroShotObjectDetection, AutoProcessor


IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff"}


def parse_args():
    p = argparse.ArgumentParser(description="Auto-label mouse images with Grounding DINO.")
    p.add_argument(
        "--input",
        type=Path,
        default=Path("data/mouse_dataset/clean/positives"),
    )
    p.add_argument(
        "--output",
        type=Path,
        default=Path("data/mouse_dataset/annotations/auto"),
    )
    p.add_argument(
        "--model",
        default="IDEA-Research/grounding-dino-tiny",
        help="Hugging Face model ID.",
    )
    p.add_argument(
        "--prompt",
        default="mouse",
        help="Grounding prompt. Single class only.",
    )
    p.add_argument(
        "--threshold",
        type=float,
        default=0.25,
        help="Detection confidence threshold.",
    )
    p.add_argument(
        "--text-threshold",
        type=float,
        default=0.25,
        help="Grounding text threshold.",
    )
    p.add_argument(
        "--nms-iou",
        type=float,
        default=0.50,
        help="Suppress duplicate boxes above this IoU.",
    )
    p.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Maximum number of images to process. 0 = all.",
    )
    p.add_argument(
        "--overwrite",
        action="store_true",
        help="Re-run images that already have annotation label files.",
    )
    p.add_argument(
        "--no-previews",
        action="store_true",
        help="Do not save annotated preview images.",
    )
    return p.parse_args()


def iou(box_a, box_b):
    ax1, ay1, ax2, ay2 = box_a
    bx1, by1, bx2, by2 = box_b

    ix1 = max(ax1, bx1)
    iy1 = max(ay1, by1)
    ix2 = min(ax2, bx2)
    iy2 = min(ay2, by2)

    iw = max(0.0, ix2 - ix1)
    ih = max(0.0, iy2 - iy1)
    inter = iw * ih

    area_a = max(0.0, ax2 - ax1) * max(0.0, ay2 - ay1)
    area_b = max(0.0, bx2 - bx1) * max(0.0, by2 - by1)
    union = area_a + area_b - inter

    return inter / union if union > 0 else 0.0


def nms_single_class(boxes, scores, threshold):
    order = sorted(range(len(scores)), key=lambda i: scores[i], reverse=True)
    kept = []

    while order:
        current = order.pop(0)
        kept.append(current)
        order = [
            i for i in order
            if iou(boxes[current], boxes[i]) <= threshold
        ]

    return kept


def clamp_box(box, width, height):
    x1, y1, x2, y2 = box
    x1 = max(0.0, min(float(width), float(x1)))
    y1 = max(0.0, min(float(height), float(y1)))
    x2 = max(0.0, min(float(width), float(x2)))
    y2 = max(0.0, min(float(height), float(y2)))

    if x2 < x1:
        x1, x2 = x2, x1
    if y2 < y1:
        y1, y2 = y2, y1

    return [x1, y1, x2, y2]


def xyxy_to_yolo(box, width, height):
    x1, y1, x2, y2 = box
    xc = ((x1 + x2) / 2.0) / width
    yc = ((y1 + y2) / 2.0) / height
    bw = (x2 - x1) / width
    bh = (y2 - y1) / height
    return xc, yc, bw, bh


def post_process(processor, outputs, inputs, image, threshold, text_threshold, prompt):
    """
    Handle both newer and older Transformers Grounding DINO APIs.
    """
    target_sizes = [(image.height, image.width)]

    # Newer Transformers API.
    try:
        return processor.post_process_grounded_object_detection(
            outputs,
            threshold=threshold,
            text_threshold=text_threshold,
            target_sizes=target_sizes,
            text_labels=[[prompt]],
        )[0]
    except TypeError:
        pass

    # Older Transformers API.
    return processor.post_process_grounded_object_detection(
        outputs,
        inputs.input_ids,
        box_threshold=threshold,
        text_threshold=text_threshold,
        target_sizes=target_sizes,
    )[0]


def get_boxes_scores(result):
    boxes = result["boxes"].detach().cpu().tolist()
    scores = result["scores"].detach().cpu().tolist()
    return boxes, scores


def draw_preview(image, boxes, scores, filename):
    preview = image.copy()
    draw = ImageDraw.Draw(preview)

    # Scale line width with image size.
    line_width = max(2, round(min(image.width, image.height) / 300))

    for box, score in zip(boxes, scores):
        x1, y1, x2, y2 = box
        draw.rectangle(
            (x1, y1, x2, y2),
            outline=(255, 0, 0),
            width=line_width,
        )

        label = f"mouse {score:.2f}"
        # Keep label inside image.
        tx = max(0, int(x1))
        ty = max(0, int(y1) - 18)
        draw.rectangle(
            (tx, ty, tx + max(85, len(label) * 8), ty + 18),
            fill=(255, 0, 0),
        )
        draw.text((tx + 3, ty + 2), label, fill=(255, 255, 255))

    draw.text(
        (8, 8),
        filename,
        fill=(255, 255, 255),
        stroke_width=2,
        stroke_fill=(0, 0, 0),
    )
    return preview


def main():
    args = parse_args()

    if not args.input.exists():
        raise SystemExit(f"Input directory not found: {args.input}")

    labels_dir = args.output / "labels"
    previews_dir = args.output / "previews"
    labels_dir.mkdir(parents=True, exist_ok=True)

    if not args.no_previews:
        previews_dir.mkdir(parents=True, exist_ok=True)

    images = sorted(
        p for p in args.input.iterdir()
        if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS
    )

    if args.limit > 0:
        images = images[:args.limit]

    if not images:
        raise SystemExit("No images found.")

    device = "cuda" if torch.cuda.is_available() else "cpu"
    print(f"Device: {device}")
    if device == "cuda":
        print(f"GPU: {torch.cuda.get_device_name(0)}")

    print(f"Loading {args.model} ...")
    processor = AutoProcessor.from_pretrained(args.model)
    model = AutoModelForZeroShotObjectDetection.from_pretrained(args.model)
    model.to(device)
    model.eval()

    manifest = []
    detection_total = 0
    no_detection_total = 0

    for index, image_path in enumerate(images, start=1):
        label_path = labels_dir / f"{image_path.stem}.txt"
        preview_path = previews_dir / f"{image_path.stem}.jpg"

        if label_path.exists() and not args.overwrite:
            print(f"[{index}/{len(images)}] skip existing: {image_path.name}")
            continue

        try:
            with Image.open(image_path) as src:
                image = ImageOps.exif_transpose(src).convert("RGB")

            text_labels = [[args.prompt]]
            inputs = processor(
                images=image,
                text=text_labels,
                return_tensors="pt",
            )
            inputs = {k: v.to(device) for k, v in inputs.items()}

            with torch.inference_mode():
                outputs = model(**inputs)

            result = post_process(
                processor,
                outputs,
                inputs,
                image,
                args.threshold,
                args.text_threshold,
                args.prompt,
            )

            boxes, scores = get_boxes_scores(result)

            # Remove invalid/zero-area boxes and clamp to source image.
            filtered_boxes = []
            filtered_scores = []

            for box, score in zip(boxes, scores):
                box = clamp_box(box, image.width, image.height)
                x1, y1, x2, y2 = box

                if (x2 - x1) < 2 or (y2 - y1) < 2:
                    continue

                filtered_boxes.append(box)
                filtered_scores.append(float(score))

            if filtered_boxes:
                keep = nms_single_class(
                    filtered_boxes,
                    filtered_scores,
                    args.nms_iou,
                )
                filtered_boxes = [filtered_boxes[i] for i in keep]
                filtered_scores = [filtered_scores[i] for i in keep]

            # Write YOLO-format proposal labels.
            lines = []
            for box in filtered_boxes:
                xc, yc, bw, bh = xyxy_to_yolo(
                    box,
                    image.width,
                    image.height,
                )
                lines.append(
                    f"0 {xc:.6f} {yc:.6f} {bw:.6f} {bh:.6f}"
                )

            label_path.write_text(
                "\n".join(lines) + ("\n" if lines else ""),
                encoding="utf-8",
            )

            if not args.no_previews:
                preview = draw_preview(
                    image,
                    filtered_boxes,
                    filtered_scores,
                    image_path.name,
                )
                preview.save(preview_path, quality=90)

            num_boxes = len(filtered_boxes)
            detection_total += num_boxes

            if num_boxes == 0:
                status = "NO_DETECTION"
                no_detection_total += 1
            elif num_boxes == 1:
                status = "ONE_BOX"
            else:
                status = "MULTIPLE_BOXES"

            best_score = max(filtered_scores) if filtered_scores else ""

            manifest.append(
                {
                    "filename": image_path.name,
                    "label_file": label_path.name,
                    "status": status,
                    "num_boxes": num_boxes,
                    "best_score": (
                        f"{best_score:.4f}"
                        if isinstance(best_score, float)
                        else ""
                    ),
                }
            )

            score_text = (
                f"{best_score:.2f}"
                if isinstance(best_score, float)
                else "-"
            )

            print(
                f"[{index}/{len(images)}] "
                f"{image_path.name}: {num_boxes} box(es), best={score_text}"
            )

        except Exception as exc:
            manifest.append(
                {
                    "filename": image_path.name,
                    "label_file": "",
                    "status": f"ERROR:{type(exc).__name__}",
                    "num_boxes": "",
                    "best_score": "",
                }
            )
            print(f"[{index}/{len(images)}] ERROR {image_path.name}: {exc}")

    manifest_path = args.output / "annotation_manifest.csv"
    fields = [
        "filename",
        "label_file",
        "status",
        "num_boxes",
        "best_score",
    ]

    with manifest_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(manifest)

    print()
    print("Auto-annotation complete")
    print("========================")
    print(f"Images attempted:   {len(manifest):,}")
    print(f"Boxes proposed:     {detection_total:,}")
    print(f"No-detection images:{no_detection_total:,}")
    print(f"Labels:             {labels_dir}")
    if not args.no_previews:
        print(f"Previews:           {previews_dir}")
    print(f"Manifest:           {manifest_path}")
    print()
    print("These are proposals only. Do not train on them before review.")


if __name__ == "__main__":
    main()
