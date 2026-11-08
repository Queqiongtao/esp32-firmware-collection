# P4 表盘字体与点阵字形生成

`main/` 和 `main/m5_assets/fonts/` 下的字体均为 LVGL 4bpp 位图字体。P4 工程以 466×466 为设计网格，再在编译期换算到 800×800 圆屏，因此字号也按同一比例放大。

## 字体清单

| 文件 | 字号 | 来源 | 用途 |
| --- | --- | --- | --- |
| `main/lv_font_clock_248.c` | 248 px | Ubuntu Medium | Minimal 主时间 |
| `main/lv_font_seg_165.c` | 165 px | DSEG7 Classic Bold | Segment 主时间 |
| `main/lv_font_seg_62.c` | 62 px | DSEG7 Classic Bold | Segment 秒数 |
| `main/lv_font_ui_62.c` | 62 px | Montserrat Medium | 菜单百分比/计时数字 |
| `main/m5_assets/fonts/CommissionerMedium185.c` | 185 px | Commissioner Medium | M5 Simple 主时间 |
| `main/m5_assets/fonts/CommissionerMedium137.c` | 137 px | Commissioner Medium | M5 NumberFlow 数字 |
| `main/m5_assets/fonts/lv_font_maple_mono_medium_48.c` | 48 px | Maple Mono Medium | M5 Classic 时间 |
| `main/m5_assets/fonts/lv_font_maple_mono_medium_41.c` | 41 px | Maple Mono Medium | M5 Classic 星期/日期 |

LVGL 内置 Montserrat 字体覆盖菜单正文和标题，`sdkconfig.defaults` 已启用 8–48 px 中实际用到的字号。

## 生成方法

### 时钟/秒表字体

```bash
DSEG_FONT=/path/to/DSEG7Classic-Bold.ttf \
MONTSERRAT_FONT=/path/to/Montserrat-Medium.ttf \
./tools/fonts/generate_clock_fonts.sh
```

- Ubuntu Medium 默认读取 `/usr/share/fonts/truetype/ubuntu/Ubuntu-M.ttf`，可用 `UBUNTU_FONT` 覆盖。
- DSEG7 Classic Bold 来自 [DSEG](https://github.com/keshikan/DSEG)，许可证见 [DSEG-LICENSE.txt](DSEG-LICENSE.txt)。
- Montserrat Medium 来自 Google Fonts，SIL Open Font License 1.1，许可证见 [Montserrat-OFL.txt](Montserrat-OFL.txt)。
- 不指定 `MONTSERRAT_FONT` 时会保留仓库中现有的 `lv_font_ui_62.c`。

### M5 字体

```bash
./tools/fonts/generate_m5_fonts.sh
```

脚本通过 `npx lv_font_conv@1.5.3` 重新生成 Commissioner 和 Maple Mono 字体，TTF 默认缓存在 `/tmp/opencode/clock-fonts`，可用 `CLOCK_FONT_CACHE_DIR` 覆盖。字体来源：

- Commissioner Medium：Google Fonts，SIL Open Font License 1.1
- Maple Mono Medium：[subframe7536/maple-font](https://github.com/subframe7536/maple-font) v7.9，SIL Open Font License 1.1

生成后执行：

```bash
./dev.sh build
./tools/preview/run.sh
```

## Dot Matrix 纯点阵字形

`main/dot_matrix_glyphs.h` 是离线采样的 10 个数字掩码，15×25、每行 15 bit。设备与主机预览直接包含此头文件，不加载 TTF。

生成脚本：

```bash
python3 tools/fonts/generate_dot_matrix_glyphs.py
```

默认读取 `/usr/share/fonts/truetype/ubuntu/UbuntuSans[wdth,wght].ttf`，宽度轴 `wdth=75`、字重轴 `wght=750`，以 32 倍分辨率渲染后面积采样到 15×25，覆盖阈值 112/255。P4 800 布局把 466 网格的 4 px 圆点/6 px 节距换算为 7 px 圆点/10 px 节距。

字体版权为 Canonical Ltd.，许可证为 [Ubuntu Font Licence 1.0](ubuntu-font-licence.txt)。
