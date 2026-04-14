#!/usr/bin/env python3

import argparse
import json
import subprocess
import sys
from pathlib import Path


DEFAULT_GRID_W = 32
DEFAULT_GRID_H = 24
DEFAULT_CROP_TOP = 60


def extract_rgb(path: Path, grid_w: int, grid_h: int, crop_top: int) -> bytes:
    vf = f"crop=iw:ih-{crop_top}:0:{crop_top},scale={grid_w}:{grid_h}:flags=area,format=rgb24"
    cmd = [
        "ffmpeg",
        "-loglevel",
        "error",
        "-i",
        str(path),
        "-vf",
        vf,
        "-frames:v",
        "1",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
        "-",
    ]
    result = subprocess.run(cmd, capture_output=True, check=True)
    expected_len = grid_w * grid_h * 3
    if len(result.stdout) != expected_len:
        raise RuntimeError(
            f"Unexpected RGB sample length for {path}: "
            f"got {len(result.stdout)}, expected {expected_len}"
        )
    return result.stdout


def mean_abs_diff(a: bytes, b: bytes) -> float:
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def mean_luma(rgb: bytes) -> float:
    total = 0.0
    for i in range(0, len(rgb), 3):
        r = rgb[i]
        g = rgb[i + 1]
        b = rgb[i + 2]
        total += 0.2126 * r + 0.7152 * g + 0.0722 * b
    return total / (len(rgb) // 3)


def dark_pixel_ratio(rgb: bytes, threshold: int = 28) -> float:
    dark = 0
    pixels = len(rgb) // 3
    for i in range(0, len(rgb), 3):
        if rgb[i] <= threshold and rgb[i + 1] <= threshold and rgb[i + 2] <= threshold:
            dark += 1
    return dark / pixels


def white_pixel_ratio(rgb: bytes, threshold: int = 235) -> float:
    bright = 0
    pixels = len(rgb) // 3
    for i in range(0, len(rgb), 3):
        if rgb[i] >= threshold and rgb[i + 1] >= threshold and rgb[i + 2] >= threshold:
            bright += 1
    return bright / pixels


def classify_scene(
    screenshot: Path,
    baselines: dict,
    grid_w: int,
    grid_h: int,
    crop_top: int,
) -> dict:
    screenshot_rgb = extract_rgb(screenshot, grid_w, grid_h, crop_top)
    distances = {}
    for scene_name, baseline_path in baselines.items():
        baseline_rgb = extract_rgb(baseline_path, grid_w, grid_h, crop_top)
        distances[scene_name] = mean_abs_diff(screenshot_rgb, baseline_rgb)

    best_scene = min(distances, key=distances.get)
    best_distance = distances[best_scene]
    luma = mean_luma(screenshot_rgb)
    dark_ratio = dark_pixel_ratio(screenshot_rgb)
    white_ratio = white_pixel_ratio(screenshot_rgb)

    if dark_ratio >= 0.82 and luma <= 35.0:
        scene = "blackscreen"
    elif white_ratio >= 0.82 and luma >= 220.0:
        scene = "whiteframe"
    elif distances["menu"] <= 18.0:
        scene = "menu"
    elif (
        distances["gameplay"] <= 16.0
        or (
            best_scene == "gameplay"
            and distances["gameplay"] <= 55.0
            and (distances["menu"] - distances["gameplay"]) >= 20.0
        )
    ):
        scene = "gameplay_loaded"
    else:
        scene = "unknown"

    return {
        "scene": scene,
        "best_scene": best_scene,
        "best_distance": round(best_distance, 3),
        "distances": {key: round(value, 3) for key, value in distances.items()},
        "mean_luma": round(luma, 3),
        "dark_ratio": round(dark_ratio, 3),
        "white_ratio": round(white_ratio, 3),
        "grid": [grid_w, grid_h],
        "crop_top": crop_top,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Classify a harness screenshot against checked-in baseline scenes."
    )
    parser.add_argument("screenshot", type=Path, help="Screenshot PNG to analyze")
    parser.add_argument(
        "--menu-baseline",
        type=Path,
        default=Path(__file__).with_name("baseline_menu.png"),
        help="Baseline menu screenshot",
    )
    parser.add_argument(
        "--gameplay-baseline",
        type=Path,
        default=Path(__file__).with_name("baseline_gameplay.png"),
        help="Baseline gameplay screenshot",
    )
    parser.add_argument(
        "--grid-width",
        type=int,
        default=DEFAULT_GRID_W,
        help="Downscaled fingerprint width",
    )
    parser.add_argument(
        "--grid-height",
        type=int,
        default=DEFAULT_GRID_H,
        help="Downscaled fingerprint height",
    )
    parser.add_argument(
        "--crop-top",
        type=int,
        default=DEFAULT_CROP_TOP,
        help="Rows to ignore from the top to avoid UI/FPS overlay noise",
    )
    parser.add_argument(
        "--expect-scene",
        choices=["menu", "gameplay_loaded", "blackscreen", "whiteframe"],
        help="Return non-zero if the classified scene does not match",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print machine-readable JSON only",
    )
    args = parser.parse_args()

    missing = [
        path for path in [args.screenshot, args.menu_baseline, args.gameplay_baseline] if not path.exists()
    ]
    if missing:
        for path in missing:
            print(f"Missing required image: {path}", file=sys.stderr)
        return 2

    baselines = {
        "menu": args.menu_baseline,
        "gameplay": args.gameplay_baseline,
    }
    result = classify_scene(
        screenshot=args.screenshot,
        baselines=baselines,
        grid_w=args.grid_width,
        grid_h=args.grid_height,
        crop_top=args.crop_top,
    )

    if args.json:
        print(json.dumps(result, sort_keys=True))
    else:
        print(
            f"Scene analysis: scene={result['scene']} "
            f"best={result['best_scene']} "
            f"distance={result['best_distance']} "
            f"distances={result['distances']}"
        )

    if args.expect_scene and result["scene"] != args.expect_scene:
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
