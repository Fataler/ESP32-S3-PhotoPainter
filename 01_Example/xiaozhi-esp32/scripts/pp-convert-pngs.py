#!/usr/bin/env python3
"""
Batch-convert PNG images for ESP32-S3-PhotoPainter / SPECTRA6 frames.

This script delegates actual image conversion to the workspace-level test.py so
the palette, rotation, BMP output, and raw .sp6 output stay identical to the
single-image workflow.

Examples:
    scripts/pp-convert-pngs.py pictures
    scripts/pp-convert-pngs.py pictures --size=800x480
    scripts/pp-convert-pngs.py pictures --size=1200x1600 --rotate-180
"""

import argparse
import subprocess
import sys
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(
        description="Convert all PNG images in a folder using test.py."
    )
    parser.add_argument("image_dir", type=Path, help="Folder with PNG images")
    parser.add_argument(
        "--size",
        default="480x800",
        help="Output image size (width x height), e.g. 480x800 or 1200x1600",
    )
    parser.add_argument(
        "--rotate-180",
        action="store_true",
        default=False,
        help="Rotate 180 degree before processing.",
    )
    parser.add_argument(
        "--rotate-angle",
        type=int,
        choices=[90, 270],
        default=270,
        help="Rotate angle if orientation mismatches. 90 or 270.",
    )
    parser.add_argument(
        "--mode",
        choices=["scale", "cut"],
        default="scale",
        help="Image conversion mode (scale or cut)",
    )
    parser.add_argument(
        "--dither",
        type=int,
        choices=[0, 3],
        default=3,
        help="Image dithering algorithm: NONE(0) or FLOYDSTEINBERG(3)",
    )
    parser.add_argument(
        "--recursive",
        action="store_true",
        default=False,
        help="Also convert PNG files from nested folders.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        default=False,
        help="Print files that would be converted without writing outputs.",
    )
    return parser.parse_args()


def find_workspace_converter():
    for parent in Path(__file__).resolve().parents:
        converter = parent / "test.py"
        if converter.is_file():
            return converter
    return None


def find_png_files(image_dir, recursive):
    pattern = "**/*.png" if recursive else "*.png"
    return sorted(path for path in image_dir.glob(pattern) if path.is_file())


def build_command(script_path, image_path, args):
    command = [
        sys.executable,
        str(script_path),
        str(image_path),
        f"--size={args.size}",
        f"--rotate-angle={args.rotate_angle}",
        f"--mode={args.mode}",
        f"--dither={args.dither}",
    ]
    if args.rotate_180:
        command.append("--rotate-180")
    return command


def main():
    args = parse_args()
    image_dir = args.image_dir.expanduser().resolve()

    if not image_dir.is_dir():
        print(f"Error: directory {image_dir} does not exist", file=sys.stderr)
        return 1

    script_path = find_workspace_converter()
    if script_path is None:
        print("Error: could not find workspace converter test.py", file=sys.stderr)
        return 1

    png_files = find_png_files(image_dir, args.recursive)
    if not png_files:
        print(f"No PNG files found in {image_dir}")
        return 0

    print(f"Found {len(png_files)} PNG file(s) in {image_dir}", flush=True)
    if args.dry_run:
        for image_path in png_files:
            print(image_path)
        return 0

    failures = 0
    for index, image_path in enumerate(png_files, start=1):
        print(f"[{index}/{len(png_files)}] {image_path}", flush=True)
        result = subprocess.run(build_command(script_path, image_path, args), check=False)
        if result.returncode != 0:
            failures += 1

    converted = len(png_files) - failures
    print(f"Converted {converted}/{len(png_files)} PNG file(s).")
    if failures:
        print(f"Failed: {failures}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
