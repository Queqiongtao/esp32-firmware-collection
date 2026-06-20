#!/usr/bin/env python3

from __future__ import annotations

import argparse
from collections import deque
import pathlib
import shutil
import subprocess
import sys
import tempfile


MOONCAT_BG_KEY = (247, 250, 250)
MOONCAT_BG_TOLERANCE = 18
MOONCAT_EDGE_CUT_TOLERANCE = 30
MOONCAT_EDGE_SOFT_TOLERANCE = 80
MOONCAT_EDGE_RADIUS = 2


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate LVGL image frames from a GIF")
    parser.add_argument("--input", required=True, help="Source GIF path")
    parser.add_argument("--output-c", required=True, help="Generated C file path")
    parser.add_argument("--output-h", required=True, help="Generated header path")
    parser.add_argument("--fps", type=int, default=0, help="Optional animation FPS after sampling")
    parser.add_argument("--width", type=int, default=180, help="Scaled frame width")
    parser.add_argument("--symbol-prefix", default="mooncat", help="Generated symbol prefix")
    return parser.parse_args()


def run_command(args: list[str]) -> None:
    subprocess.run(args, check=True)


def read_frame_durations_ms(ffprobe: str, input_path: pathlib.Path) -> list[int]:
    result = subprocess.run(
        [
            ffprobe,
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "frame=pkt_duration_time,duration_time",
            "-of",
            "csv=p=0",
            str(input_path),
        ],
        check=True,
        capture_output=True,
        text=True,
    )

    durations: list[int] = []
    for line in result.stdout.splitlines():
        value = line.strip().split(",", 1)[0]
        if not value:
            continue
        duration_ms = int(round(float(value) * 1000.0))
        durations.append(max(duration_ms, 1))

    return durations


def background_delta(red: int, green: int, blue: int) -> int:
    return max(
        abs(red - MOONCAT_BG_KEY[0]),
        abs(green - MOONCAT_BG_KEY[1]),
        abs(blue - MOONCAT_BG_KEY[2]),
    )


def has_transparent_neighbor(pixels: bytearray, width: int, height: int, x: int, y: int) -> bool:
    x_min = max(0, x - MOONCAT_EDGE_RADIUS)
    x_max = min(width - 1, x + MOONCAT_EDGE_RADIUS)
    y_min = max(0, y - MOONCAT_EDGE_RADIUS)
    y_max = min(height - 1, y + MOONCAT_EDGE_RADIUS)

    for sample_y in range(y_min, y_max + 1):
        for sample_x in range(x_min, x_max + 1):
            if sample_x == x and sample_y == y:
                continue

            offset = (sample_y * width + sample_x) * 4
            if pixels[offset + 3] == 0:
                return True

    return False


def find_foreground_edge_color(
    pixels: bytearray,
    width: int,
    height: int,
    x: int,
    y: int,
) -> tuple[int, int, int] | None:
    red_total = 0
    green_total = 0
    blue_total = 0
    sample_count = 0

    x_min = max(0, x - MOONCAT_EDGE_RADIUS)
    x_max = min(width - 1, x + MOONCAT_EDGE_RADIUS)
    y_min = max(0, y - MOONCAT_EDGE_RADIUS)
    y_max = min(height - 1, y + MOONCAT_EDGE_RADIUS)

    for sample_y in range(y_min, y_max + 1):
        for sample_x in range(x_min, x_max + 1):
            if sample_x == x and sample_y == y:
                continue

            offset = (sample_y * width + sample_x) * 4
            alpha = pixels[offset + 3]
            if alpha == 0:
                continue

            red = pixels[offset]
            green = pixels[offset + 1]
            blue = pixels[offset + 2]
            if background_delta(red, green, blue) <= MOONCAT_EDGE_SOFT_TOLERANCE:
                continue

            red_total += red
            green_total += green
            blue_total += blue
            sample_count += 1

    if sample_count == 0:
        return None

    return red_total // sample_count, green_total // sample_count, blue_total // sample_count


def clean_background_fringe(pixels: bytearray, width: int, height: int) -> None:
    source = pixels[:]

    for y in range(height):
        for x in range(width):
            offset = (y * width + x) * 4
            alpha = source[offset + 3]
            if alpha == 0:
                continue
            if not has_transparent_neighbor(source, width, height, x, y):
                continue

            red = source[offset]
            green = source[offset + 1]
            blue = source[offset + 2]
            delta = background_delta(red, green, blue)
            if delta > MOONCAT_EDGE_SOFT_TOLERANCE:
                continue

            if delta <= MOONCAT_EDGE_CUT_TOLERANCE:
                pixels[offset + 3] = 0
                continue

            foreground = find_foreground_edge_color(source, width, height, x, y)
            if foreground is not None:
                pixels[offset] = foreground[0]
                pixels[offset + 1] = foreground[1]
                pixels[offset + 2] = foreground[2]

            keep_ratio = (delta - MOONCAT_EDGE_CUT_TOLERANCE) / (
                MOONCAT_EDGE_SOFT_TOLERANCE - MOONCAT_EDGE_CUT_TOLERANCE
            )
            pixels[offset + 3] = min(alpha, int(alpha * keep_ratio))


def clear_connected_background(raw: bytes, width: int, height: int) -> bytes:
    pixels = bytearray(raw)
    visited = bytearray(width * height)
    queue: deque[tuple[int, int]] = deque()

    def is_background(x: int, y: int) -> bool:
        offset = (y * width + x) * 4
        red = pixels[offset]
        green = pixels[offset + 1]
        blue = pixels[offset + 2]
        alpha = pixels[offset + 3]

        return (
            alpha >= 240
            and background_delta(red, green, blue) <= MOONCAT_BG_TOLERANCE
        )

    def enqueue_if_background(x: int, y: int) -> None:
        index = y * width + x
        if visited[index] or not is_background(x, y):
            return

        visited[index] = 1
        queue.append((x, y))

    for x in range(width):
        enqueue_if_background(x, 0)
        enqueue_if_background(x, height - 1)

    for y in range(1, height - 1):
        enqueue_if_background(0, y)
        enqueue_if_background(width - 1, y)

    while queue:
        x, y = queue.popleft()
        offset = (y * width + x) * 4
        pixels[offset + 3] = 0

        if x > 0:
            enqueue_if_background(x - 1, y)
        if x + 1 < width:
            enqueue_if_background(x + 1, y)
        if y > 0:
            enqueue_if_background(x, y - 1)
        if y + 1 < height:
            enqueue_if_background(x, y + 1)

    clean_background_fringe(pixels, width, height)

    return bytes(pixels)


def parse_pam(path: pathlib.Path) -> tuple[int, int, bytes]:
    headers: dict[str, str] = {}

    with path.open("rb") as handle:
        magic = handle.readline().strip()
        if magic != b"P7":
            raise ValueError(f"{path} is not a PAM image")

        while True:
            line = handle.readline()
            if not line:
                raise ValueError(f"{path} has an incomplete PAM header")

            line = line.strip()
            if not line or line.startswith(b"#"):
                continue
            if line == b"ENDHDR":
                break

            key, value = line.split(None, 1)
            headers[key.decode("ascii").lower()] = value.decode("ascii")

        width = int(headers["width"])
        height = int(headers["height"])
        depth = int(headers["depth"])
        tuple_type = headers.get("tupltype") or headers.get("tupletype")
        if depth != 4 or tuple_type != "RGB_ALPHA":
            raise ValueError(f"{path} is expected to be RGBA PAM, got depth={depth}, tuple={tuple_type}")

        raw = handle.read()

    expected_size = width * height * 4
    if len(raw) != expected_size:
        raise ValueError(f"{path} payload size mismatch: {len(raw)} != {expected_size}")

    raw = clear_connected_background(raw, width, height)

    # LVGL v9 lv_color32_t is laid out as BGRA in memory.
    bgra = bytearray(expected_size)
    for index in range(0, expected_size, 4):
        red = raw[index]
        green = raw[index + 1]
        blue = raw[index + 2]
        alpha = raw[index + 3]

        bgra[index] = blue
        bgra[index + 1] = green
        bgra[index + 2] = red
        bgra[index + 3] = alpha

    return width, height, bytes(bgra)


def format_c_array(data: bytes) -> str:
    lines: list[str] = []
    row: list[str] = []

    for index, value in enumerate(data, start=1):
        row.append(f"0x{value:02X}")
        if index % 16 == 0:
            lines.append("    " + ", ".join(row) + ",")
            row = []

    if row:
        lines.append("    " + ", ".join(row) + ",")

    return "\n".join(lines)


def generate_files(
    frames: list[tuple[int, int, bytes]],
    frame_durations_ms: list[int],
    output_c: pathlib.Path,
    output_h: pathlib.Path,
    symbol_prefix: str,
) -> None:
    frame_names = [f"{symbol_prefix}_frame_{index:02d}" for index in range(len(frames))]

    header_text = f"""#pragma once

#include <stddef.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {{
#endif

extern const lv_image_dsc_t * const g_{symbol_prefix}_frames[];
extern const uint32_t g_{symbol_prefix}_frame_durations_ms[];
extern const size_t g_{symbol_prefix}_frame_count;

#ifdef __cplusplus
}}
#endif
"""

    c_parts = [
        f'#include "{output_h.name}"',
        "",
    ]

    for frame_name, (width, height, data) in zip(frame_names, frames):
        stride = width * 4
        data_name = f"{frame_name}_map"
        c_parts.extend(
            [
                f"LV_ATTRIBUTE_MEM_ALIGN static const uint8_t {data_name}[] = {{",
                format_c_array(data),
                "};",
                "",
                f"static const lv_image_dsc_t {frame_name} = {{",
                "    .header = {",
                "        .magic = LV_IMAGE_HEADER_MAGIC,",
                "        .cf = LV_COLOR_FORMAT_ARGB8888,",
                "        .flags = 0,",
                f"        .w = {width},",
                f"        .h = {height},",
                f"        .stride = {stride},",
                "        .reserved_2 = 0,",
                "    },",
                f"    .data_size = sizeof({data_name}),",
                f"    .data = {data_name},",
                "    .reserved = NULL,",
                "    .reserved_2 = NULL,",
                "};",
                "",
            ]
        )

    c_parts.extend(
        [
            f"const lv_image_dsc_t * const g_{symbol_prefix}_frames[] = {{",
            *[f"    &{frame_name}," for frame_name in frame_names],
            "};",
            "",
            f"const uint32_t g_{symbol_prefix}_frame_durations_ms[] = {{",
            *[f"    {duration}," for duration in frame_durations_ms],
            "};",
            "",
            f"const size_t g_{symbol_prefix}_frame_count = sizeof(g_{symbol_prefix}_frames) / sizeof(g_{symbol_prefix}_frames[0]);",
            "",
        ]
    )

    output_h.write_text(header_text, encoding="utf-8")
    output_c.write_text("\n".join(c_parts), encoding="utf-8")


def main() -> int:
    args = parse_args()
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        print("ffmpeg is required to generate mooncat frames", file=sys.stderr)
        return 1
    ffprobe = shutil.which("ffprobe")
    if ffprobe is None:
        print("ffprobe is required to read mooncat frame durations", file=sys.stderr)
        return 1

    input_path = pathlib.Path(args.input)
    output_c = pathlib.Path(args.output_c)
    output_h = pathlib.Path(args.output_h)

    output_c.parent.mkdir(parents=True, exist_ok=True)
    output_h.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="mooncat_frames_") as temp_dir_name:
        temp_dir = pathlib.Path(temp_dir_name)
        frame_pattern = temp_dir / "frame_%02d.pam"
        frame_durations_ms = read_frame_durations_ms(ffprobe, input_path)
        if args.fps > 0:
            filter_graph = f"fps={args.fps},scale={args.width}:-1:flags=lanczos"
        else:
            filter_graph = f"scale={args.width}:-1:flags=lanczos"

        run_command(
            [
                ffmpeg,
                "-v",
                "error",
                "-y",
                "-i",
                str(input_path),
                "-vf",
                filter_graph,
                "-pix_fmt",
                "rgba",
                str(frame_pattern),
            ]
        )

        frame_paths = sorted(temp_dir.glob("frame_*.pam"))
        if not frame_paths:
            raise RuntimeError("No frames were generated from GIF")

        frames = [parse_pam(path) for path in frame_paths]
        if args.fps > 0:
            frame_durations_ms = [max(int(round(1000.0 / args.fps)), 1)] * len(frames)
        elif len(frame_durations_ms) != len(frames):
            raise RuntimeError(
                f"Frame duration count mismatch: {len(frame_durations_ms)} != {len(frames)}"
            )

        generate_files(frames, frame_durations_ms, output_c, output_h, args.symbol_prefix)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
