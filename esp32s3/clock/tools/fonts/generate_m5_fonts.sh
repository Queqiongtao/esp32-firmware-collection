#!/usr/bin/env bash
# 重新生成 M5 表盘字体（4bpp）。
#
# M5 上游字体是 2bpp，大字号下边缘只有 4 级灰度，108px/80px 数字看起来会发软。
# 本脚本用同一批 TTF、相同字号和字符集重新生成 4bpp 字体，不改变布局。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLOCK_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
FONT_DIR="$CLOCK_DIR/main/m5_assets/fonts"
CACHE_DIR="${CLOCK_FONT_CACHE_DIR:-/tmp/opencode/clock-fonts}"
GEN_DIR="$CACHE_DIR/generated"
LV_FONT_CONV="lv_font_conv@1.5.3"

COMMISSIONER_URL="${COMMISSIONER_URL:-https://fonts.gstatic.com/s/commissioner/v24/tDaH2o2WnlgI0FNDgduEk4jAhwgumbU1SVfU5BD8OuRL8OstC6KOhgvBYWSFJ-Mgdrgiju6fF8meZm0rk4eF-ZugTPFdGPc.ttf}"
MAPLE_URL="${MAPLE_URL:-https://github.com/subframe7536/maple-font/releases/download/v7.9/MapleMono-TTF.zip}"

need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "缺少依赖: $1" >&2
        exit 1
    fi
}

need npx
need curl
need unzip
need python3

mkdir -p "$CACHE_DIR" "$GEN_DIR"

if [[ ! -f "$CACHE_DIR/Commissioner-Medium.ttf" ]]; then
    echo "下载 Commissioner-Medium.ttf ..."
    curl -fL --retry 3 --connect-timeout 20 --max-time 300 \
        -o "$CACHE_DIR/Commissioner-Medium.ttf" "$COMMISSIONER_URL"
fi

if [[ ! -f "$CACHE_DIR/MapleMono-Medium.ttf" ]]; then
    if [[ ! -f "$CACHE_DIR/MapleMono-TTF.zip" ]]; then
        echo "下载 MapleMono-TTF.zip ..."
        curl -fL --retry 3 --connect-timeout 20 --max-time 300 \
            -o "$CACHE_DIR/MapleMono-TTF.zip" "$MAPLE_URL"
    fi
    rm -rf "$CACHE_DIR/maple"
    unzip -o -q "$CACHE_DIR/MapleMono-TTF.zip" -d "$CACHE_DIR/maple"
    cp "$CACHE_DIR/maple/MapleMono-Medium.ttf" "$CACHE_DIR/MapleMono-Medium.ttf"
fi

cd "$GEN_DIR"
cp "$CACHE_DIR/Commissioner-Medium.ttf" .
cp "$CACHE_DIR/MapleMono-Medium.ttf" .

# Commissioner：只包含时间需要的数字、冒号、括号和句点。
npx --yes "$LV_FONT_CONV" \
    --bpp 4 --size 108 --no-compress \
    --font Commissioner-Medium.ttf \
    --symbols '0123456789:().' \
    --format lvgl --lv-font-name CommissionerMedium108 \
    -o CommissionerMedium108.c

npx --yes "$LV_FONT_CONV" \
    --bpp 4 --size 80 --no-compress \
    --font Commissioner-Medium.ttf \
    --symbols '0123456789' \
    --format lvgl --lv-font-name CommissionerMedium80 \
    -o CommissionerMedium80.c

# Maple Mono：保留原字体 ASCII 范围。
npx --yes "$LV_FONT_CONV" \
    --bpp 4 --size 24 --no-compress \
    --font MapleMono-Medium.ttf \
    --range 32-127 \
    --format lvgl --lv-font-name lv_font_maple_mono_medium_24 \
    -o lv_font_maple_mono_medium_24.c

npx --yes "$LV_FONT_CONV" \
    --bpp 4 --size 28 --no-compress \
    --font MapleMono-Medium.ttf \
    --range 32-127 \
    --format lvgl --lv-font-name lv_font_maple_mono_medium_28 \
    -o lv_font_maple_mono_medium_28.c

# lv_font_conv 生成的 include 块是 LVGL 8 风格；工程里统一使用
# __has_include 兼容块，并补充字体来源和许可证说明。
python3 - "$GEN_DIR" <<'PY'
import sys
from pathlib import Path

out_dir = Path(sys.argv[1])
old_inc = '''#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif'''
new_inc = '''#ifdef __has_include
#if __has_include("lvgl.h")
#ifndef LV_LVGL_H_INCLUDE_SIMPLE
#define LV_LVGL_H_INCLUDE_SIMPLE
#endif
#endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif'''
sources = {
    "CommissionerMedium108.c": "Commissioner-Medium.ttf, Google Fonts, SIL Open Font License 1.1",
    "CommissionerMedium80.c": "Commissioner-Medium.ttf, Google Fonts, SIL Open Font License 1.1",
    "lv_font_maple_mono_medium_24.c": "MapleMono-Medium.ttf, Maple Mono, SIL Open Font License 1.1",
    "lv_font_maple_mono_medium_28.c": "MapleMono-Medium.ttf, Maple Mono, SIL Open Font License 1.1",
}
marker = " ******************************************************************************/"
for name, source in sources.items():
    path = out_dir / name
    text = path.read_text()
    if old_inc not in text:
        raise SystemExit(f"{name}: 找不到预期的 include 块")
    text = text.replace(old_inc, new_inc, 1)
    if marker not in text:
        raise SystemExit(f"{name}: 找不到文件头结束标记")
    text = text.replace(marker, f" * Source: {source}\n{marker}", 1)
    path.write_text(text.rstrip() + "\n")
PY

cp "$GEN_DIR/CommissionerMedium108.c" "$FONT_DIR/CommissionerMedium108.c"
cp "$GEN_DIR/CommissionerMedium80.c" "$FONT_DIR/CommissionerMedium80.c"
cp "$GEN_DIR/lv_font_maple_mono_medium_24.c" "$FONT_DIR/lv_font_maple_mono_medium_24.c"
cp "$GEN_DIR/lv_font_maple_mono_medium_28.c" "$FONT_DIR/lv_font_maple_mono_medium_28.c"

echo "已更新:"
echo "  $FONT_DIR/CommissionerMedium108.c"
echo "  $FONT_DIR/CommissionerMedium80.c"
echo "  $FONT_DIR/lv_font_maple_mono_medium_24.c"
echo "  $FONT_DIR/lv_font_maple_mono_medium_28.c"
echo
echo "下一步: ./dev.sh build && ./tools/preview/run.sh"
