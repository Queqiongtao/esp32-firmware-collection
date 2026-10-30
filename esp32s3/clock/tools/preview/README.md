# 主题预览工具

主机端渲染 `main/clock_theme_*.cpp` 的离屏预览，用于快速检查构图、对齐、裁切和配色，**不参与 ESP-IDF 构建**。

预览工具和固件编译的是同一批主题源码，但入口、LVGL 配置、时间源、显示后端和截图对象不同；它是主机端近似预览，不是固件本体。详细的设计边界和差异说明见 [`../../doc/preview-tool.md`](../../doc/preview-tool.md)。

## 前置条件

- 首次运行前至少执行一次 `./dev.sh build`，让 `managed_components/lvgl__lvgl` 下载到位。
- 需要 CMake、支持 C++17 的主机编译器和 Python 3。
- `render.py` 依赖 Pillow，仓库没有提供 `requirements.txt`，需要自行安装：

  ```bash
  python3 -m pip install Pillow
  ```

## 用法

```bash
./tools/preview/run.sh
./tools/preview/run.sh --time 23:59:59
```

`run.sh` 会依次执行 CMake 配置、编译、清理上一轮产物、运行 `build/preview`，最后调用 `render.py`。

`--time HH:MM:SS` 只渲染指定时刻。`run.sh` 会把参数同时传给 `preview` 和 `render.py`，因此对比图会使用同一个时刻；如果手动多次执行 `build/preview --time ...`，最后运行 `render.py` 时也要传入匹配的 `--time`。

## 文件职责

| 文件 | 说明 |
|------|------|
| `run.sh` | 一键构建、清理产物、运行 preview、调用 render.py |
| `CMakeLists.txt` | 主机端 LVGL 和主题源码构建配置 |
| `preview.cpp` | 虚拟 466 × 466 display、主题遍历、固定时间、截图和 PPM 输出 |
| `lv_conf.h` | 预览专用 LVGL 配置，关键项尽量与设备端对齐 |
| `render.py` | PPM 转 PNG、圆形遮罩、对比图和越界报告 |

## 产物

产物在 `tools/preview/out/`，默认直接看 `out/round/`：

| 路径 | 说明 |
|------|------|
| `out/round/*.png` | **默认查看**：圆形遮罩 + 亮灰玻璃边缘，圆外为中灰 |
| `out/raw/*.png` | 未遮罩方图，仅用于调试构图 |
| `out/ppm/*.ppm` | 原始渲染（466 × 466 RGB565 转 RGB888） |
| `out/contact_native.png` | 4 个原生主题圆屏对比图 |
| `out/contact_m5.png` | 4 个 M5 主题圆屏对比图 |
| `out/contact_all.png` | 全部 8 个主题圆屏对比图 |
| `out/report.txt` | 非黑像素范围与安全区越界提示 |

`report.txt` 中的告警含义：

- 最大半径超过 233：`⚠ 超出圆屏`。
- 最大半径超过安全半径 205：`⚠ 超出安全区`。
- 告警只是构图提示，不是渲染失败；外圈刻度、边框等元素本来就可能贴近边缘。

`run.sh` 每次运行都会清空上一轮的 `ppm/raw/round` 产物和对比图，避免不同时刻的旧图混进对比图。需要额外渲染边界时刻时，可以手动多次执行 `build/preview --time HH:MM:SS`，再运行一次 `render.py`。

## 原理

1. `CMakeLists.txt` 通过 LVGL 自带的 `env_support/cmake/os_desktop.cmake` 把 `managed_components/lvgl__lvgl` 编成主机库。
2. `lv_conf.h` 是关键配置，需与设备端对齐：RGB565、Montserrat 12~40 子集、SNAPSHOT/SCALE/CANVAS/ARC 等。
3. `preview.cpp` 创建 466 × 466 虚拟 display，逐个主题 `build/update/tick` 后用 `lv_snapshot_take(screen)` 取整屏像素。
4. `render.py` 负责 PPM → PNG、圆形遮罩和拼图。

## 与固件的主要差异

- preview 只编译主题、字体/图片资源和 LVGL；不编译 `main.cpp`、设置菜单、触摸手势、RTC、IMU 和主题过渡。
- preview 使用固定日期 2026-10-20 UTC，默认渲染 10:08:35 和 23:59:35；固件使用 PCF85063 RTC 和本地时间。
- preview 把 `tick()` 一次性推进到未来 5 秒和 6 秒；固件按 33 ms 周期逐帧调用。
- preview 截 `lv_screen_active()`；固件截主题 root 到复用 draw buffer，再按 IMU 姿态旋转。
- preview 使用独立的 `lv_conf.h`；固件使用 `sdkconfig`，额外启用压缩字体、更多字号和双绘制单元等。
- preview 不调用 `set_rotation()` / `set_overlay_visible()`，不覆盖旋转、overlay 显隐和切换过渡。

完整的对照表、能力边界和维护约定见 [`../../doc/preview-tool.md`](../../doc/preview-tool.md)。

## 注意

- 快照对象是 `lv_screen_active()`，因为 M5 Simple 的秒点等元素挂在 screen 上而不是主题根。
- 预览只验证构图与配色；亮度、Gamma、刷新流畅度仍需上板确认。
- 新增主题时，除 `main/main.cpp` 外，还要把主题注册到 `tools/preview/preview.cpp` 的 `kThemes`。
- 新增顶层字体时，需要同时加入 `main/CMakeLists.txt` 和 `tools/preview/CMakeLists.txt`；M5 字体和图片在 preview 中由 GLOB 自动纳入，但设备端仍需显式添加。
- 修改 `main/` 下主题源码后重新执行 `run.sh` 即可，无需刷机。
