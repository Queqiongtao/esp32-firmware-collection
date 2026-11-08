#!/usr/bin/env bash
# 重新生成 P4 800x800 时钟主题字体。
#
# 需要 lv_font_conv（npx 首次运行会自动下载）以及以下 TTF：
#   UBUNTU_FONT      Ubuntu Medium，用于 minimal 248px 和 UI 62px
#   DSEG_FONT        DSEG7 Classic Bold，用于 segment 165/62px
#   MONTSERRAT_FONT  Montserrat Medium，用于 lv_font_ui_62（可选）
#
# 默认优先使用系统 Ubuntu 字体；DSEG/Montserrat 可用环境变量指定。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLOCK_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
GEN_DIR="${CLOCK_FONT_GEN_DIR:-/tmp/opencode/p4-clock-fonts}"
LV_FONT_CONV="lv_font_conv@1.5.3"

UBUNTU_FONT="${UBUNTU_FONT:-/usr/share/fonts/truetype/ubuntu/Ubuntu-M.ttf}"
DSEG_FONT="${DSEG_FONT:-}"
MONTSERRAT_FONT="${MONTSERRAT_FONT:-}"

need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "缺少依赖: $1" >&2
        exit 1
    fi
}

need npx
need python3

if [[ ! -f "$UBUNTU_FONT" ]]; then
    echo "找不到 Ubuntu Medium: $UBUNTU_FONT" >&2
    echo "可用 UBUNTU_FONT=/path/to/Ubuntu-M.ttf 指定。" >&2
    exit 1
fi

if [[ -z "$DSEG_FONT" || ! -f "$DSEG_FONT" ]]; then
    echo "找不到 DSEG7 Classic Bold TTF。" >&2
    echo "请下载 DSEG 字体并用 DSEG_FONT=/path/to/DSEG7Classic-Bold.ttf 指定。" >&2
    exit 1
fi

mkdir -p "$GEN_DIR"
cd "$GEN_DIR"

# Minimal 主时间：数字 + 冒号。
npx --yes "$LV_FONT_CONV" \
    --bpp 4 --size 248 --no-compress \
    --font "$UBUNTU_FONT" \
    --range 0x30-0x3A \
    --format lvgl --lv-font-name lv_font_clock_248 \
    --lv-include lvgl.h \
    -o lv_font_clock_248.c

# Segment 主时间和小秒数。
npx --yes "$LV_FONT_CONV" \
    --bpp 4 --size 165 --no-compress \
    --font "$DSEG_FONT" \
    --range 0x30-0x3A \
    --format lvgl --lv-font-name lv_font_seg_165 \
    --lv-include lvgl.h \
    -o lv_font_seg_165.c

npx --yes "$LV_FONT_CONV" \
    --bpp 4 --size 62 --no-compress \
    --font "$DSEG_FONT" \
    --range 0x2D-0x3A \
    --format lvgl --lv-font-name lv_font_seg_62 \
    --lv-include lvgl.h \
    -o lv_font_seg_62.c

# 菜单的大号百分比/计时数字。
if [[ -n "$MONTSERRAT_FONT" && -f "$MONTSERRAT_FONT" ]]; then
    npx --yes "$LV_FONT_CONV" \
        --bpp 4 --size 62 --no-compress \
        --font "$MONTSERRAT_FONT" \
        --range 32-127 \
        --format lvgl --lv-font-name lv_font_ui_62 \
        --lv-include lvgl.h \
        -o lv_font_ui_62.c
    cp "$GEN_DIR/lv_font_ui_62.c" "$CLOCK_DIR/main/lv_font_ui_62.c"
else
    echo "未指定 MONTSERRAT_FONT，跳过 lv_font_ui_62.c（保留仓库现有文件）。"
fi

cp "$GEN_DIR/lv_font_clock_248.c" "$CLOCK_DIR/main/lv_font_clock_248.c"
cp "$GEN_DIR/lv_font_seg_165.c" "$CLOCK_DIR/main/lv_font_seg_165.c"
cp "$GEN_DIR/lv_font_seg_62.c" "$CLOCK_DIR/main/lv_font_seg_62.c"

echo "已更新:"
echo "  $CLOCK_DIR/main/lv_font_clock_248.c"
echo "  $CLOCK_DIR/main/lv_font_seg_165.c"
echo "  $CLOCK_DIR/main/lv_font_seg_62.c"
echo
echo "下一步: ./dev.sh build && ./tools/preview/run.sh"
