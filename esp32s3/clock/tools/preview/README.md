# 主题预览工具

主机端渲染 `main/clock_theme_*.cpp` 的离屏预览，用于快速检查构图、对齐、裁切和配色，**不参与 ESP-IDF 构建**。

## 用法

```bash
./tools/preview/run.sh
./tools/preview/run.sh --time 23:59:59
```

产物在 `tools/preview/out/`，默认直接看 `out/round/`：

| 路径 | 说明 |
|------|------|
| `out/round/*.png` | **默认查看**：圆形遮罩 + 亮灰玻璃边缘，圆外为中灰 |
| `out/raw/*.png` | 未遮罩方图，仅用于调试构图 |
| `out/ppm/*.ppm` | 原始渲染（466 x 466 RGB565 转 RGB888） |
| `out/contact_native.png` | 4 个原生主题圆屏对比图 |
| `out/contact_m5.png` | 4 个 M5 主题圆屏对比图 |
| `out/contact_all.png` | 全部 8 个主题圆屏对比图 |
| `out/report.txt` | 非黑像素范围与安全区越界提示 |

每次运行会清空上一轮 `ppm/raw/round` 产物；额外渲染边界时刻时，可直接多次执行 `build/preview --time HH:MM:SS`，再运行一次 `render.py`。

## 原理

1. `CMakeLists.txt` 通过 LVGL 自带的 `env_support/cmake/os_desktop.cmake` 把 `managed_components/lvgl__lvgl` 编成主机库。
2. `lv_conf.h` 是关键配置，需与设备端对齐：RGB565、Montserrat 12~40、SNAPSHOT/SCALE/CANVAS/ARC 等。
3. `preview.cpp` 创建 466 x 466 虚拟 display，逐个主题 `build/update/tick` 后用 `lv_snapshot_take(screen)` 取整屏像素。
4. `render.py` 负责 PPM → PNG、圆形遮罩和拼图。

## 注意

- 快照对象是 `lv_screen_active()`，因为 M5 Simple 的秒点等元素挂在 screen 上而不是主题根。
- 预览只验证构图与配色；亮度、Gamma、刷新流畅度仍需上板确认。
- 新增主题字体时，需要同时加入 `main/CMakeLists.txt` 和 `tools/preview/CMakeLists.txt`。
- 修改 `main/` 下主题源码后重新执行 `run.sh` 即可，无需刷机。
