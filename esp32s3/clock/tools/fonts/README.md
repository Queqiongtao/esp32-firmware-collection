# M5 表盘字体生成

`main/m5_assets/fonts/` 下的 Commissioner / Maple Mono 字体来自 M5StopWatch-UserDemo。上游资源是 **2bpp**，只有 4 级灰度；`m5_simple` 的 108px 数字和 `m5_number_flow` 的 80px 数字在这种 bpp 下边缘会明显发软。

本目录的脚本用原始 TTF、相同字号和字符集重新生成 **4bpp** 字体，只提升抗锯齿精度，不改变字号、字宽和布局。

## 生成文件

```text
main/m5_assets/fonts/CommissionerMedium108.c
main/m5_assets/fonts/CommissionerMedium80.c
main/m5_assets/fonts/lv_font_maple_mono_medium_24.c
main/m5_assets/fonts/lv_font_maple_mono_medium_28.c
```

保持不变的字体：

- `main/lv_font_clock_144.c`：Minimal 主题，Ubuntu Medium，144px，4bpp
- `main/lv_font_seg_96.c`、`main/lv_font_seg_36.c`：Segment 主题，DSEG7，4bpp
- LVGL 内置 Montserrat 字体：4bpp

## 使用方法

在 `esp32s3/clock` 目录执行：

```bash
./tools/fonts/generate_m5_fonts.sh
./dev.sh build
./tools/preview/run.sh
```

脚本依赖：

- `npx`（首次运行会下载 `lv_font_conv@1.5.3`）
- `curl`、`unzip`、`python3`

TTF 默认缓存在 `/tmp/opencode/clock-fonts`，可以用环境变量覆盖：

```bash
CLOCK_FONT_CACHE_DIR=/path/to/fonts ./tools/fonts/generate_m5_fonts.sh
```

脚本不会把 TTF 复制进仓库，也不会自动提交生成结果。

## 字体来源

- Commissioner Medium 500
  - 来源：Google Fonts
  - 许可证：SIL Open Font License 1.1
  - 脚本通过 `fonts.gstatic.com` 下载静态 TTF
- Maple Mono Medium
  - 来源：https://github.com/subframe7536/maple-font
  - 当前使用 v7.9 的 `MapleMono-TTF.zip`
  - 许可证：SIL Open Font License 1.1

如果上游 TTF 版本变化，个别字形的 `box_h` 可能存在 1px 级差异；不会改变 `adv_w` 和整体布局。升级字体版本后应重新跑预览，重点检查 `m5_simple`、`m5_number_flow` 和 `m5_classic`。

## 说明

- `m5_number_flow` 当前使用 80px 数字和 28px 日期，是工程的显示设计；M5 上游对应的是 64px 数字和 24px 日期。
- `CommissionerMedium64.c` 目前没有主题使用，保留作为上游资源参考，不参与 4bpp 重新生成。
