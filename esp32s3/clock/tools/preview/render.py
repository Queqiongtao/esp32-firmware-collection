#!/usr/bin/env python3
"""把 preview 生成的 PPM 转成 PNG，并叠加圆屏遮罩与安全区参考。

输出：
  out/raw/*.png      原始渲染
  out/r1.0/*.png     圆形遮罩 + 玻璃边缘（模拟真实面板）
  out/contact.png    8 个主题对比图
  out/report.txt     每个主题的非黑像素外接范围与越界告警
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

SIZE = 466
CENTER = SIZE // 2
RADIUS = SIZE // 2
SAFE_RADIUS = 205


def circular_mask(image: Image.Image, bezel: bool = True) -> Image.Image:
    """把圆形外的区域涂黑，可选画一圈玻璃边缘。"""
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).ellipse(
        (CENTER - RADIUS, CENTER - RADIUS, CENTER + RADIUS - 1, CENTER + RADIUS - 1),
        fill=255,
    )
    out = image.copy()
    out.putalpha(mask)
    background = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 255))
    out = Image.alpha_composite(background, out).convert("RGB")

    if bezel:
        draw = ImageDraw.Draw(out)
        draw.ellipse(
            (CENTER - RADIUS + 1, CENTER - RADIUS + 1, CENTER + RADIUS - 2, CENTER + RADIUS - 2),
            outline=(42, 42, 42),
            width=2,
        )
    return out


def content_report(name: str, image: Image.Image) -> str:
    """统计非黑像素的最远半径，超出安全区时给出提示。"""
    pixels = image.load()
    min_x = min_y = SIZE
    max_x = max_y = -1
    max_radius = 0.0

    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b = pixels[x, y]
            if r + g + b <= 24:
                continue
            min_x = min(min_x, x)
            min_y = min(min_y, y)
            max_x = max(max_x, x)
            max_y = max(max_y, y)
            dx = x - CENTER
            dy = y - CENTER
            max_radius = max(max_radius, (dx * dx + dy * dy) ** 0.5)

    if max_x < 0:
        return f"{name}: 全黑"

    note = ""
    if max_radius > RADIUS:
        note = "  ⚠ 超出圆屏"
    elif max_radius > SAFE_RADIUS:
        note = "  ⚠ 超出安全区"
    return (
        f"{name}: x[{min_x},{max_x}] y[{min_y},{max_y}] "
        f"最大半径 {max_radius:.1f}{note}"
    )


def contact_sheet(images: list[tuple[str, Image.Image]], columns: int, output: Path) -> None:
    label_height = 26
    rows = (len(images) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * SIZE, rows * (SIZE + label_height)), (18, 18, 18))
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default()

    for index, (name, image) in enumerate(images):
        column = index % columns
        row = index // columns
        sheet.paste(image, (column * SIZE, row * (SIZE + label_height) + label_height))
        draw.text((column * SIZE + 8, row * (SIZE + label_height) + 6), name, fill=(200, 200, 200), font=font)

    sheet.save(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", default="out", help="PPM 目录")
    parser.add_argument("--time", default="10-08-35", help="用于对比图的时刻标签")
    args = parser.parse_args()

    input_dir = Path(args.input)
    if not input_dir.is_absolute():
        input_dir = Path(__file__).parent / input_dir

    raw_dir = input_dir / "raw"
    masked_dir = input_dir / "r1.0"
    raw_dir.mkdir(parents=True, exist_ok=True)
    masked_dir.mkdir(parents=True, exist_ok=True)

    ppm_files = sorted(input_dir.glob("*.ppm"))
    if not ppm_files:
        raise SystemExit(f"no PPM files under {input_dir}, run ./build/preview first")

    reports: list[str] = []
    sheet_items: list[tuple[str, Image.Image]] = []

    for ppm in ppm_files:
        image = Image.open(ppm).convert("RGB")
        name = ppm.stem
        raw_path = raw_dir / f"{name}.png"
        image.save(raw_path)
        masked = circular_mask(image)
        masked_path = masked_dir / f"{name}.png"
        masked.save(masked_path)

        reports.append(content_report(name, image))

        if name.endswith(args.time):
            theme = name[: -(len(args.time) + 1)]
            sheet_items.append((theme, masked))

    sheet_items.sort(key=lambda item: item[0])
    native = [item for item in sheet_items if not item[0].startswith("m5_")]
    m5 = [item for item in sheet_items if item[0].startswith("m5_")]

    if native:
        contact_sheet(native, 2, input_dir / "contact_native.png")
    if m5:
        contact_sheet(m5, 2, input_dir / "contact_m5.png")
    if sheet_items:
        contact_sheet(sheet_items, 4, input_dir / "contact_all.png")

    report_path = input_dir / "report.txt"
    report_path.write_text("\n".join(reports) + "\n", encoding="utf-8")
    print("\n".join(reports))
    print(f"\ncontact sheet: {input_dir / 'contact_native.png'}")


if __name__ == "__main__":
    main()
