# 表盘字体与点阵字形生成

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

## Dot Matrix 纯点阵字形

`main/dot_matrix_glyphs.h` 是离线采样的 10 个数字掩码，15×25、每行 15 bit，总计 500 字节。设备与主机预览直接包含此头文件，不加载 TTF，也无需新增 LVGL 字体或修改 CMake。

生成源为系统 Ubuntu 字体包的 `UbuntuSans[wdth,wght].ttf`，Version 1.006；宽度轴 `wdth=75`、字重轴 `wght=750`。十个数字共用缩放、基线和固定字格，按字宽居中，不单独拉伸；以 32 倍分辨率渲染后面积采样到 15×25，覆盖阈值为 112/255。主时间用 4 px 圆点、2 px 间距显示，仅绘制命中点。

在工程目录执行（依赖 Python 3 与 Pillow，脚本不下载字体）：

```bash
python3 tools/fonts/generate_dot_matrix_glyphs.py
# 也可显式指定同一字体文件：
python3 tools/fonts/generate_dot_matrix_glyphs.py --font '/path/to/UbuntuSans[wdth,wght].ttf'
./tools/preview/run.sh
python3 tools/preview/generate_thumbnails.py
./tools/preview/run.sh
./dev.sh build
```

默认读取 `/usr/share/fonts/truetype/ubuntu/UbuntuSans[wdth,wght].ttf`；生成头文件记录源文件名、SHA-256、采样参数和版权信息。相同源字体和 Pillow 环境可重复生成相同掩码。TTF 不入库，字体版本变化后需检查全部数字，尤其是 0/6/8/9 和连续的 1/2。

字体版权为 Canonical Ltd.（2011、2022、2023），许可证为 [Ubuntu Font Licence 1.0](ubuntu-font-licence.txt)；生成的数字子集标注为 Ubuntu Sans derivative Clock Dot Matrix。
