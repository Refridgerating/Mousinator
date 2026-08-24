#!/usr/bin/env python3
"""
Non-destructive mechanical cleanup for Sentry mouse-positive images.

What it does:
- Verifies that each image can be decoded.
- Records dimensions, file size, SHA-256, and perceptual hash.
- Excludes exact duplicate files while retaining one canonical copy.
- Flags near-duplicate images using perceptual-hash Hamming distance.
- Flags very small and extreme-aspect-ratio images.
- Copies valid, non-exact-duplicate images into clean/positives.
- Never modifies or deletes anything in raw/positives.

Default input:
    data/mouse_dataset/raw/positives

Default outputs:
    data/mouse_dataset/clean/positives
    data/mouse_dataset/cleanup/cleanup_report.csv
    data/mouse_dataset/cleanup/near_duplicate_pairs.csv
    data/mouse_dataset/cleanup/summary.txt
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import shutil
from collections import Counter, defaultdict
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageOps


IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff"}


def sha256_file(path: Path, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(chunk_size):
            digest.update(chunk)
    return digest.hexdigest()


def open_normalized(path: Path) -> Image.Image:
    """Load image and apply EXIF orientation in memory only."""
    with Image.open(path) as im:
        im.load()
        normalized = ImageOps.exif_transpose(im).convert("RGB")
    return normalized


def phash64(image: Image.Image) -> int:
    """
    64-bit perceptual hash based on low-frequency DCT coefficients.
    Similar images generally have small Hamming distance.
    """
    gray = image.convert("L").resize((32, 32), Image.Resampling.LANCZOS)
    array = np.asarray(gray, dtype=np.float32)
    dct = cv2.dct(array)
    low = dct[:8, :8].flatten()

    # Ignore the DC coefficient when calculating the median threshold.
    median = float(np.median(low[1:]))
    bits = low > median

    value = 0
    for bit in bits:
        value = (value << 1) | int(bool(bit))
    return value


def hamming(a: int, b: int) -> int:
    return (a ^ b).bit_count()


class UnionFind:
    def __init__(self, items):
        self.parent = {item: item for item in items}

    def find(self, item):
        root = item
        while self.parent[root] != root:
            root = self.parent[root]

        while self.parent[item] != item:
            parent = self.parent[item]
            self.parent[item] = root
            item = parent

        return root

    def union(self, a, b):
        ra = self.find(a)
        rb = self.find(b)
        if ra != rb:
            self.parent[rb] = ra


def parse_args():
    parser = argparse.ArgumentParser(
        description="Mechanical cleanup of Sentry mouse-positive images."
    )
    parser.add_argument(
        "--input",
        type=Path,
        default=Path("data/mouse_dataset/raw/positives"),
        help="Raw positive-image directory.",
    )
    parser.add_argument(
        "--clean-dir",
        type=Path,
        default=Path("data/mouse_dataset/clean/positives"),
        help="Derived clean-image directory.",
    )
    parser.add_argument(
        "--report-dir",
        type=Path,
        default=Path("data/mouse_dataset/cleanup"),
        help="Directory for CSV reports and summary.",
    )
    parser.add_argument(
        "--min-side",
        type=int,
        default=200,
        help="Flag images if either oriented dimension is below this many pixels.",
    )
    parser.add_argument(
        "--max-aspect",
        type=float,
        default=4.0,
        help="Flag images with width/height or height/width above this value.",
    )
    parser.add_argument(
        "--phash-distance",
        type=int,
        default=4,
        help="Flag perceptual-hash pairs at or below this Hamming distance.",
    )
    return parser.parse_args()


def main():
    args = parse_args()

    if not args.input.exists():
        raise SystemExit(f"Input directory does not exist: {args.input}")

    images = sorted(
        p for p in args.input.iterdir()
        if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS
    )

    if not images:
        raise SystemExit(f"No supported images found in: {args.input}")

    args.report_dir.mkdir(parents=True, exist_ok=True)

    # clean/ is derived output. Rebuild it each run so stale files cannot survive.
    if args.clean_dir.exists():
        shutil.rmtree(args.clean_dir)
    args.clean_dir.mkdir(parents=True, exist_ok=True)

    records = []
    hash_owner = {}
    valid_unique = []

    print(f"Scanning {len(images):,} images...")

    for index, path in enumerate(images, start=1):
        record = {
            "filename": path.name,
            "source_path": str(path),
            "clean_path": "",
            "file_bytes": path.stat().st_size,
            "width": "",
            "height": "",
            "aspect_ratio": "",
            "sha256": "",
            "phash64": "",
            "status": "",
            "flags": "",
            "exact_duplicate_of": "",
            "near_duplicate_group": "",
        }

        flags = []

        try:
            image = open_normalized(path)
            width, height = image.size

            record["width"] = width
            record["height"] = height
            aspect = max(width / height, height / width)
            record["aspect_ratio"] = f"{aspect:.4f}"

            if min(width, height) < args.min_side:
                flags.append("small")

            if aspect > args.max_aspect:
                flags.append("extreme_aspect")

            sha = sha256_file(path)
            record["sha256"] = sha

            if sha in hash_owner:
                record["status"] = "exclude_exact_duplicate"
                record["exact_duplicate_of"] = hash_owner[sha]
                flags.append("exact_duplicate")
            else:
                hash_owner[sha] = path.name
                p_hash = phash64(image)
                record["phash64"] = f"{p_hash:016x}"
                record["_phash_int"] = p_hash
                record["_path"] = path
                valid_unique.append(record)

        except Exception as exc:
            record["status"] = "exclude_unreadable"
            flags.append("unreadable")
            flags.append(type(exc).__name__)

        record["flags"] = "|".join(flags)
        records.append(record)

        if index % 100 == 0 or index == len(images):
            print(f"  {index:,}/{len(images):,}")

    # Find near duplicates among valid, exact-unique images.
    print(
        f"Comparing {len(valid_unique):,} valid unique images "
        f"for near duplicates (pHash <= {args.phash_distance})..."
    )

    names = [r["filename"] for r in valid_unique]
    uf = UnionFind(names)
    near_pairs = []

    for i in range(len(valid_unique)):
        a = valid_unique[i]
        for j in range(i + 1, len(valid_unique)):
            b = valid_unique[j]
            distance = hamming(a["_phash_int"], b["_phash_int"])
            if distance <= args.phash_distance:
                near_pairs.append(
                    {
                        "file_a": a["filename"],
                        "file_b": b["filename"],
                        "phash_distance": distance,
                    }
                )
                uf.union(a["filename"], b["filename"])

    groups = defaultdict(list)
    for name in names:
        groups[uf.find(name)].append(name)

    duplicate_groups = [
        sorted(group) for group in groups.values() if len(group) > 1
    ]
    duplicate_groups.sort(key=lambda g: (-len(g), g[0]))

    group_id_by_file = {}
    for group_number, group in enumerate(duplicate_groups, start=1):
        group_id = f"ND{group_number:04d}"
        for filename in group:
            group_id_by_file[filename] = group_id

    # Finalize valid records and copy them into clean/positives.
    for record in valid_unique:
        filename = record["filename"]
        flags = [x for x in record["flags"].split("|") if x]

        if filename in group_id_by_file:
            record["near_duplicate_group"] = group_id_by_file[filename]
            flags.append("near_duplicate")

        record["flags"] = "|".join(sorted(set(flags)))

        if record["flags"]:
            record["status"] = "keep_flagged"
        else:
            record["status"] = "keep"

        destination = args.clean_dir / filename
        shutil.copy2(record["_path"], destination)
        record["clean_path"] = str(destination)

    # Strip internal-only fields.
    report_fields = [
        "filename",
        "source_path",
        "clean_path",
        "file_bytes",
        "width",
        "height",
        "aspect_ratio",
        "sha256",
        "phash64",
        "status",
        "flags",
        "exact_duplicate_of",
        "near_duplicate_group",
    ]

    report_path = args.report_dir / "cleanup_report.csv"
    with report_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=report_fields)
        writer.writeheader()
        for record in records:
            writer.writerow({k: record.get(k, "") for k in report_fields})

    pairs_path = args.report_dir / "near_duplicate_pairs.csv"
    with pairs_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["file_a", "file_b", "phash_distance"],
        )
        writer.writeheader()
        writer.writerows(
            sorted(
                near_pairs,
                key=lambda r: (r["phash_distance"], r["file_a"], r["file_b"]),
            )
        )

    counts = Counter(r["status"] for r in records)
    flag_counts = Counter()
    for record in records:
        for flag in record.get("flags", "").split("|"):
            if flag:
                flag_counts[flag] += 1

    summary_lines = [
        "Sentry mouse-positive mechanical cleanup",
        "=" * 42,
        f"Raw images scanned:       {len(images):,}",
        f"Kept in clean/positives:  {counts['keep'] + counts['keep_flagged']:,}",
        f"  clean / unflagged:      {counts['keep']:,}",
        f"  kept but flagged:       {counts['keep_flagged']:,}",
        f"Exact duplicates excluded:{counts['exclude_exact_duplicate']:,}",
        f"Unreadable excluded:      {counts['exclude_unreadable']:,}",
        f"Near-duplicate groups:    {len(duplicate_groups):,}",
        f"Near-duplicate pairs:     {len(near_pairs):,}",
        "",
        "Flags:",
    ]

    if flag_counts:
        for flag, count in sorted(flag_counts.items()):
            summary_lines.append(f"  {flag:22s} {count:,}")
    else:
        summary_lines.append("  none")

    summary_lines.extend(
        [
            "",
            f"Report: {report_path}",
            f"Near-duplicate pairs: {pairs_path}",
            f"Clean images: {args.clean_dir}",
            "",
            "Nothing in raw/positives was modified or deleted.",
        ]
    )

    summary = "\n".join(summary_lines)
    (args.report_dir / "summary.txt").write_text(summary, encoding="utf-8")

    print()
    print(summary)


if __name__ == "__main__":
    main()
