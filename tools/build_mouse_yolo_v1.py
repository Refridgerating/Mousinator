#!/usr/bin/env python3
"""
Build a deterministic YOLO dataset from the reviewed Sentry mouse annotations.

Inputs
------
data/mouse_dataset/clean/positives
data/mouse_dataset/annotations/final/labels
data/mouse_dataset/cleanup/cleanup_report.csv   (optional, used to group near duplicates)

Output
------
data/mouse_dataset/yolo_v1/
    images/train, val, test
    labels/train, val, test
    data.yaml
    split_manifest.csv
    summary.txt

Notes
-----
- Empty label files are valid negative images.
- Near-duplicate groups are kept in the same split.
- Split is deterministic with --seed.
"""

from __future__ import annotations

import argparse
import csv
import random
import shutil
from collections import Counter, defaultdict
from pathlib import Path

IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff"}


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--images", type=Path, default=Path("data/mouse_dataset/clean/positives"))
    p.add_argument("--labels", type=Path, default=Path("data/mouse_dataset/annotations/final/labels"))
    p.add_argument("--cleanup-report", type=Path, default=Path("data/mouse_dataset/cleanup/cleanup_report.csv"))
    p.add_argument("--output", type=Path, default=Path("data/mouse_dataset/yolo_v1"))
    p.add_argument("--train", type=float, default=0.80)
    p.add_argument("--val", type=float, default=0.10)
    p.add_argument("--test", type=float, default=0.10)
    p.add_argument("--seed", type=int, default=26)
    return p.parse_args()


def read_near_duplicate_groups(path: Path) -> dict[str, str]:
    groups = {}
    if not path.exists():
        return groups
    with path.open("r", newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            gid = (row.get("near_duplicate_group") or "").strip()
            if gid:
                groups[row["filename"]] = gid
    return groups


def label_is_positive(path: Path) -> bool:
    return bool(path.read_text(encoding="utf-8").strip())


def choose_split(counts, targets, group_size):
    # Prefer the split with the largest remaining fractional deficit.
    best = None
    best_score = None
    for split in ("train", "val", "test"):
        deficit = targets[split] - counts[split]
        score = deficit / max(targets[split], 1)
        # Penalize avoidable overshoot.
        if deficit < group_size:
            score -= (group_size - deficit) / max(targets[split], 1)
        if best_score is None or score > best_score:
            best = split
            best_score = score
    return best


def assign_groups(groups, ratios, seed):
    rng = random.Random(seed)
    total = sum(len(items) for items in groups.values())
    targets = {
        "train": round(total * ratios["train"]),
        "val": round(total * ratios["val"]),
    }
    targets["test"] = total - targets["train"] - targets["val"]

    # Stratify by whether a group contains any positives / negatives.
    strata = defaultdict(list)
    for gid, items in groups.items():
        pos = sum(1 for x in items if x["positive"])
        neg = len(items) - pos
        if pos and neg:
            key = "mixed"
        elif pos:
            key = "positive"
        else:
            key = "negative"
        strata[key].append((gid, items))

    assignment = {}
    counts = Counter()

    # Distribute each stratum independently to preserve class balance.
    for stratum in ("mixed", "negative", "positive"):
        entries = strata[stratum]
        rng.shuffle(entries)
        # Larger groups first reduces late overshoot.
        entries.sort(key=lambda x: len(x[1]), reverse=True)

        stratum_total = sum(len(items) for _, items in entries)
        stratum_targets = {
            "train": round(stratum_total * ratios["train"]),
            "val": round(stratum_total * ratios["val"]),
        }
        stratum_targets["test"] = stratum_total - stratum_targets["train"] - stratum_targets["val"]
        stratum_counts = Counter()

        for gid, items in entries:
            split = choose_split(stratum_counts, stratum_targets, len(items))
            assignment[gid] = split
            stratum_counts[split] += len(items)
            counts[split] += len(items)

    return assignment


def main():
    args = parse_args()

    if abs((args.train + args.val + args.test) - 1.0) > 1e-9:
        raise SystemExit("train + val + test must equal 1.0")

    if not args.images.exists() or not args.labels.exists():
        raise SystemExit("Input image or label directory does not exist.")

    near_groups = read_near_duplicate_groups(args.cleanup_report)

    images = sorted(
        p for p in args.images.iterdir()
        if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS
    )
    if not images:
        raise SystemExit("No images found.")

    records = []
    groups = defaultdict(list)

    for image in images:
        label = args.labels / f"{image.stem}.txt"
        if not label.exists():
            raise SystemExit(f"Missing final label for {image.name}")

        positive = label_is_positive(label)
        gid = near_groups.get(image.name, f"FILE:{image.name}")

        item = {
            "filename": image.name,
            "image": image,
            "label": label,
            "positive": positive,
            "group": gid,
        }
        records.append(item)
        groups[gid].append(item)

    ratios = {"train": args.train, "val": args.val, "test": args.test}
    assignment = assign_groups(groups, ratios, args.seed)

    if args.output.exists():
        shutil.rmtree(args.output)

    for split in ("train", "val", "test"):
        (args.output / "images" / split).mkdir(parents=True, exist_ok=True)
        (args.output / "labels" / split).mkdir(parents=True, exist_ok=True)

    summary_counts = defaultdict(Counter)
    manifest_rows = []

    for item in records:
        split = assignment[item["group"]]
        dst_image = args.output / "images" / split / item["filename"]
        dst_label = args.output / "labels" / split / item["label"].name

        shutil.copy2(item["image"], dst_image)
        shutil.copy2(item["label"], dst_label)

        kind = "positive" if item["positive"] else "negative"
        summary_counts[split]["images"] += 1
        summary_counts[split][kind] += 1

        manifest_rows.append({
            "filename": item["filename"],
            "split": split,
            "positive": "yes" if item["positive"] else "no",
            "group": item["group"],
        })

    yaml_text = """path: .
train: images/train
val: images/val
test: images/test

names:
  0: mouse
"""
    (args.output / "data.yaml").write_text(yaml_text, encoding="utf-8")

    with (args.output / "split_manifest.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=["filename", "split", "positive", "group"])
        writer.writeheader()
        writer.writerows(sorted(manifest_rows, key=lambda r: (r["split"], r["filename"])))

    lines = [
        "Sentry mouse YOLO v1 split",
        "=" * 28,
        f"Seed: {args.seed}",
        "",
    ]
    for split in ("train", "val", "test"):
        c = summary_counts[split]
        lines.append(
            f"{split:5s}: {c['images']:4d} images  "
            f"{c['positive']:4d} positive  {c['negative']:3d} negative"
        )

    lines.extend([
        "",
        f"Near-duplicate groups kept together: {len([g for g in groups if not g.startswith('FILE:')])}",
        f"Dataset: {args.output}",
    ])
    summary = "\n".join(lines)
    (args.output / "summary.txt").write_text(summary, encoding="utf-8")
    print(summary)


if __name__ == "__main__":
    main()
