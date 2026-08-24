#!/usr/bin/env python3
"""Build a licensed iNaturalist mouse-positive image pool from a GBIF Darwin Core ZIP.

This script never calls the iNaturalist API. It reads the GBIF archive locally and,
when requested, downloads selected media URLs from the archive.

Default policy:
- dataset: iNaturalist Research-grade observations
- taxon: genus Mus
- media: JPEG still images only
- licenses: CC0, CC BY 4.0, CC BY-SA 4.0
- one image per GBIF/iNaturalist observation
- image size: iNaturalist "large" where possible
- downloader: 500 images/run, 1 s delay, 1 GiB/run cap
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import os
import re
import shutil
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from typing import Dict, Iterable, List, Optional
from urllib.parse import urlparse

# GBIF dataset key for iNaturalist Research-grade Observations.
INAT_DATASET_KEY = "50c9509d-22c7-4a22-a47d-8c48425ef4a7"

ALLOWED_MEDIA_LICENSES = {
    "http://creativecommons.org/publicdomain/zero/1.0/": "CC0-1.0",
    "https://creativecommons.org/publicdomain/zero/1.0/": "CC0-1.0",
    "http://creativecommons.org/licenses/by/4.0/": "CC-BY-4.0",
    "https://creativecommons.org/licenses/by/4.0/": "CC-BY-4.0",
    "http://creativecommons.org/licenses/by-sa/4.0/": "CC-BY-SA-4.0",
    "https://creativecommons.org/licenses/by-sa/4.0/": "CC-BY-SA-4.0",
}

MANIFEST_FIELDS = [
    "candidate_rank",
    "filename",
    "gbif_id",
    "observation_id",
    "photo_id",
    "dataset_class",
    "scientific_name",
    "species",
    "genus",
    "country_code",
    "state_province",
    "event_date",
    "observation_url",
    "photo_url",
    "download_url",
    "creator",
    "rights_holder",
    "media_license",
    "media_license_url",
    "gbif_occurrence_license",
]


def _set_csv_field_limit() -> None:
    """Allow Darwin Core rows with unusually large text fields."""
    limit = sys.maxsize
    while True:
        try:
            csv.field_size_limit(limit)
            return
        except OverflowError:
            limit //= 10


def parse_numeric_tail(url: str, marker: str) -> str:
    match = re.search(rf"/{re.escape(marker)}/(\d+)", url or "")
    return match.group(1) if match else ""


def to_inat_size(url: str, size: str = "large") -> str:
    """Convert an iNaturalist hosted original image URL to a requested size."""
    if size == "original":
        return url
    parsed = urlparse(url)
    if parsed.netloc not in {
        "inaturalist-open-data.s3.amazonaws.com",
        "static.inaturalist.org",
    }:
        return url
    return re.sub(r"/original(?=\.)", f"/{size}", url, count=1)


def stable_rank(seed: int, gbif_id: str) -> str:
    # A hex digest sorts deterministically but avoids archive/order bias.
    return hashlib.sha256(f"{seed}:{gbif_id}".encode("utf-8")).hexdigest()


def load_occurrences(dwca_zip: Path) -> Dict[str, dict]:
    observations: Dict[str, dict] = {}
    with zipfile.ZipFile(dwca_zip) as zf:
        if "occurrence.txt" not in zf.namelist():
            raise RuntimeError("ZIP does not contain occurrence.txt")

        with zf.open("occurrence.txt") as raw:
            reader = csv.DictReader(io.TextIOWrapper(raw, encoding="utf-8"), delimiter="\t")
            for row in reader:
                if row.get("datasetKey") != INAT_DATASET_KEY:
                    continue
                if row.get("genus") != "Mus":
                    continue

                gbif_id = row.get("gbifID", "").strip()
                if not gbif_id:
                    continue

                observations[gbif_id] = {
                    "gbif_id": gbif_id,
                    "scientific_name": row.get("scientificName", ""),
                    "species": row.get("species", ""),
                    "genus": row.get("genus", ""),
                    "country_code": row.get("countryCode", ""),
                    "state_province": row.get("stateProvince", ""),
                    "event_date": row.get("eventDate", ""),
                    "observation_url": row.get("references", ""),
                    "gbif_occurrence_license": row.get("license", ""),
                }

    return observations


def choose_media(dwca_zip: Path, observations: Dict[str, dict], image_size: str) -> Dict[str, dict]:
    """Choose one allowed JPEG image per observation."""
    chosen: Dict[str, dict] = {}

    with zipfile.ZipFile(dwca_zip) as zf:
        if "multimedia.txt" not in zf.namelist():
            raise RuntimeError("ZIP does not contain multimedia.txt")

        with zf.open("multimedia.txt") as raw:
            reader = csv.DictReader(io.TextIOWrapper(raw, encoding="utf-8"), delimiter="\t")
            for row in reader:
                gbif_id = row.get("gbifID", "").strip()
                if gbif_id not in observations or gbif_id in chosen:
                    continue
                if row.get("type") != "StillImage":
                    continue
                if row.get("format") != "image/jpeg":
                    continue

                license_url = (row.get("license") or "").strip()
                short_license = ALLOWED_MEDIA_LICENSES.get(license_url)
                if not short_license:
                    continue

                photo_url = (row.get("identifier") or "").strip()
                if not photo_url.startswith(("http://", "https://")):
                    continue

                chosen[gbif_id] = {
                    "photo_id": parse_numeric_tail(row.get("references", ""), "photos")
                    or parse_numeric_tail(photo_url, "photos"),
                    "photo_url": photo_url,
                    "download_url": to_inat_size(photo_url, image_size),
                    "creator": row.get("creator", ""),
                    "rights_holder": row.get("rightsHolder", ""),
                    "media_license": short_license,
                    "media_license_url": license_url,
                }

    return chosen


def build_manifest(dwca_zip: Path, output_root: Path, seed: int, image_size: str) -> Path:
    _set_csv_field_limit()
    metadata_dir = output_root / "metadata"
    metadata_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = metadata_dir / "mouse_positive_manifest.csv"

    observations = load_occurrences(dwca_zip)
    media = choose_media(dwca_zip, observations, image_size)

    records: List[dict] = []
    for gbif_id, media_row in media.items():
        obs = observations[gbif_id]
        observation_id = parse_numeric_tail(obs.get("observation_url", ""), "observations")
        photo_id = media_row.get("photo_id", "")
        ext = Path(urlparse(media_row["download_url"]).path).suffix.lower()
        if ext not in {".jpg", ".jpeg"}:
            ext = ".jpg"

        identity = observation_id or gbif_id
        suffix = photo_id or "photo"
        filename = f"inat_obs_{identity}_photo_{suffix}{ext}"

        records.append(
            {
                "_rank": stable_rank(seed, gbif_id),
                "filename": filename,
                "gbif_id": gbif_id,
                "observation_id": observation_id,
                "photo_id": photo_id,
                "dataset_class": "mouse",
                **obs,
                **media_row,
            }
        )

    records.sort(key=lambda r: r["_rank"])

    with manifest_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=MANIFEST_FIELDS)
        writer.writeheader()
        for idx, row in enumerate(records, start=1):
            row["candidate_rank"] = idx
            writer.writerow({key: row.get(key, "") for key in MANIFEST_FIELDS})

    print(f"iNaturalist Mus occurrences in archive: {len(observations):,}")
    print(f"Usable observations after media/license filtering: {len(records):,}")
    print(f"Manifest: {manifest_path}")
    return manifest_path


def read_manifest(path: Path) -> List[dict]:
    with path.open("r", newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def append_error(error_path: Path, row: dict, message: str) -> None:
    exists = error_path.exists()
    with error_path.open("a", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["timestamp", "filename", "gbif_id", "download_url", "error"],
        )
        if not exists:
            writer.writeheader()
        writer.writerow(
            {
                "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
                "filename": row.get("filename", ""),
                "gbif_id": row.get("gbif_id", ""),
                "download_url": row.get("download_url", ""),
                "error": message,
            }
        )


def download_one(row: dict, destination: Path, user_agent: str, timeout: int) -> int:
    temp_path = destination.with_suffix(destination.suffix + ".part")
    req = urllib.request.Request(
        row["download_url"],
        headers={
            "User-Agent": user_agent,
            "Accept": "image/jpeg,image/*;q=0.8,*/*;q=0.1",
        },
    )

    with urllib.request.urlopen(req, timeout=timeout) as response, temp_path.open("wb") as out:
        content_type = response.headers.get("Content-Type", "")
        if content_type and not content_type.lower().startswith("image/"):
            raise RuntimeError(f"unexpected Content-Type: {content_type}")
        shutil.copyfileobj(response, out, length=1024 * 1024)

    size = temp_path.stat().st_size
    if size < 1024:
        temp_path.unlink(missing_ok=True)
        raise RuntimeError(f"downloaded file suspiciously small: {size} bytes")

    # JPEG sanity check.
    with temp_path.open("rb") as f:
        if f.read(3)[:2] != b"\xff\xd8":
            temp_path.unlink(missing_ok=True)
            raise RuntimeError("downloaded file does not look like a JPEG")

    os.replace(temp_path, destination)
    return size


def download_manifest(
    manifest_path: Path,
    output_root: Path,
    max_images: int,
    max_bytes: int,
    delay: float,
    timeout: int,
    retries: int,
    user_agent: str,
) -> None:
    records = read_manifest(manifest_path)
    image_dir = output_root / "raw" / "positives"
    metadata_dir = output_root / "metadata"
    image_dir.mkdir(parents=True, exist_ok=True)
    metadata_dir.mkdir(parents=True, exist_ok=True)
    error_path = metadata_dir / "download_errors.csv"

    downloaded = 0
    bytes_this_run = 0
    already_present = 0

    for row in records:
        if downloaded >= max_images:
            break
        if bytes_this_run >= max_bytes:
            print("Run byte cap reached.")
            break

        destination = image_dir / row["filename"]
        if destination.exists() and destination.stat().st_size >= 1024:
            already_present += 1
            continue

        last_error: Optional[Exception] = None
        for attempt in range(retries + 1):
            try:
                size = download_one(row, destination, user_agent, timeout)
                downloaded += 1
                bytes_this_run += size
                print(
                    f"[{downloaded:4d}/{max_images}] {row['filename']} "
                    f"({size / 1024:.0f} KiB, total {bytes_this_run / (1024**2):.1f} MiB)"
                )
                last_error = None
                break
            except urllib.error.HTTPError as exc:
                last_error = exc
                # Respect throttling and transient server failures.
                if exc.code == 429:
                    backoff = max(60.0, delay * (2 ** (attempt + 1)))
                elif 500 <= exc.code < 600:
                    backoff = max(10.0, delay * (2 ** (attempt + 1)))
                else:
                    break
                print(f"HTTP {exc.code}; backing off {backoff:.0f}s")
                time.sleep(backoff)
            except (urllib.error.URLError, TimeoutError, OSError, RuntimeError) as exc:
                last_error = exc
                if attempt < retries:
                    backoff = max(5.0, delay * (2 ** (attempt + 1)))
                    print(f"Download error; retrying in {backoff:.0f}s: {exc}")
                    time.sleep(backoff)

        if last_error is not None:
            append_error(error_path, row, repr(last_error))
            print(f"FAILED: {row['filename']}: {last_error}")

        # Intentional pacing even though these are media URLs, not API calls.
        time.sleep(delay)

    print("\nRun complete")
    print(f"New images: {downloaded:,}")
    print(f"Already present/skipped: {already_present:,}")
    print(f"Downloaded this run: {bytes_this_run / (1024**2):.1f} MiB")
    print(f"Image directory: {image_dir}")


def show_stats(manifest_path: Path) -> None:
    rows = read_manifest(manifest_path)
    from collections import Counter

    species = Counter(row["species"] or row["scientific_name"] for row in rows)
    countries = Counter(row["country_code"] or "UNKNOWN" for row in rows)
    licenses = Counter(row["media_license"] for row in rows)

    print(f"Candidates: {len(rows):,}\n")
    print("Licenses:")
    for name, count in licenses.most_common():
        print(f"  {name:16s} {count:5d}")
    print("\nTop species:")
    for name, count in species.most_common(12):
        print(f"  {name[:35]:35s} {count:5d}")
    print("\nTop countries:")
    for name, count in countries.most_common(15):
        print(f"  {name:8s} {count:5d}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    prepare = sub.add_parser("prepare", help="Build the mouse-positive manifest from a GBIF DwC-A ZIP")
    prepare.add_argument("dwca_zip", type=Path)
    prepare.add_argument("--output", type=Path, default=Path("data/mouse_dataset"))
    prepare.add_argument("--seed", type=int, default=20260823)
    prepare.add_argument("--image-size", choices=["small", "medium", "large", "original"], default="large")

    download = sub.add_parser("download", help="Download a paced batch from an existing manifest")
    download.add_argument("--manifest", type=Path, default=Path("data/mouse_dataset/metadata/mouse_positive_manifest.csv"))
    download.add_argument("--output", type=Path, default=Path("data/mouse_dataset"))
    download.add_argument("--max-images", type=int, default=500)
    download.add_argument("--max-gib", type=float, default=1.0, help="Maximum data downloaded in this run")
    download.add_argument("--delay", type=float, default=1.0, help="Seconds between image requests")
    download.add_argument("--timeout", type=int, default=60)
    download.add_argument("--retries", type=int, default=2)
    download.add_argument("--user-agent", default="SentryComputerVisionDataset/0.1")

    stats = sub.add_parser("stats", help="Summarize an existing manifest")
    stats.add_argument("--manifest", type=Path, default=Path("data/mouse_dataset/metadata/mouse_positive_manifest.csv"))
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    if args.command == "prepare":
        if not args.dwca_zip.exists():
            parser.error(f"ZIP not found: {args.dwca_zip}")
        build_manifest(args.dwca_zip, args.output, args.seed, args.image_size)
    elif args.command == "download":
        if not args.manifest.exists():
            parser.error(f"Manifest not found: {args.manifest}. Run 'prepare' first.")
        if args.max_images < 1:
            parser.error("--max-images must be >= 1")
        if args.max_gib <= 0:
            parser.error("--max-gib must be > 0")
        if args.delay < 0:
            parser.error("--delay must be >= 0")
        download_manifest(
            manifest_path=args.manifest,
            output_root=args.output,
            max_images=args.max_images,
            max_bytes=int(args.max_gib * 1024**3),
            delay=args.delay,
            timeout=args.timeout,
            retries=args.retries,
            user_agent=args.user_agent,
        )
    elif args.command == "stats":
        if not args.manifest.exists():
            parser.error(f"Manifest not found: {args.manifest}")
        show_stats(args.manifest)


if __name__ == "__main__":
    main()
