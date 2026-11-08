# 主题与菜单预览工具

主机端渲染 P4 800×800 圆屏的时钟主题和设置菜单离屏预览，用于快速检查构图、对齐、裁切和配色，**不参与 ESP-IDF 构建**。WiFi 页、配网摘要页和 SSID/密码编辑页用静态状态渲染；Power 页只显示 USB 供电和息屏选项，不模拟电池。

预览工具和固件编译同一批主题源码，但入口、LVGL 配置、时间源、显示后端和截图对象不同；它是主机端近似预览，不是固件本体。设计边界见 [`../../doc/preview-tool.md`](../../doc/preview-tool.md)。

## 前置条件

- 首次运行前至少执行一次 `./dev.sh build`，让 `managed_components/lvgl__lvgl` 下载到位。
- 需要 CMake、支持 C++17 的主机编译器和 Python 3。
- `render.py` 依赖 Pillow：

  ```bash
  python3 -m pip install Pillow
  ```

## 用法

```bash
./tools/preview/run.sh
./tools/preview/run.sh --time 23:59:59
```

`run.sh` 会依次执行 CMake 配置、编译、清理上一轮产物、运行 `build/preview`，最后调用 `render.py`。

## 产物

产物在 `tools/preview/out/`：

| 路径 | 说明 |
| --- | --- |
| `out/round/*.png` | 圆形遮罩 + 玻璃边缘，默认查看 |
| `out/raw/*.png` | 未遮罩方图 |
| `out/ppm/*.ppm` | 原始 800×800 渲染 |
| `out/contact_native.png` / `contact_m5.png` / `contact_all.png` | 主题对比图 |
| `out/menu/*.png` | 设置菜单页面（圆屏遮罩） |
| `out/menu/raw/*.png` | 菜单未遮罩方图 |
| `out/menu/contact_menu.png` | 菜单页面/弹层对比图 |
| `out/menu/report.txt` | 菜单内容最大半径与越界提示 |
| `out/report.txt` | 主题非黑像素范围与安全区提示 |

`safe radius` 为 352 px（466 设计的 205 px 等比换算）；接近 400 px 的边框/刻度会提示“超出安全区”，这是构图提示而非渲染失败。

## 原理

1. `CMakeLists.txt` 通过 LVGL 自带的 `env_support/cmake/os_desktop.cmake` 把 `managed_components/lvgl__lvgl` 编成主机库。
2. `lv_conf.h` 对齐固件关键配置：RGB565、Montserrat 8–48、SNAPSHOT/CANVAS/ARC 等。
3. `preview.cpp` 创建 800×800 虚拟 display，逐个主题 `build/update/tick` 后用 `lv_snapshot_take(screen)` 取整屏像素；随后渲染 21 个菜单状态，包括 4 页主菜单、表盘选择器、WiFi 配网/扫描/键盘、Sound 弹层等。
4. `render.py` 负责 PPM → PNG、圆形遮罩、拼图和越界报告。

## 与固件的主要差异

- preview 只编译主题、设置菜单、字体/图片资源和 LVGL；不编译 `main.cpp`、触摸手势、RTC、ESP-Hosted 和主题过渡。
- preview 使用固定日期 2026-10-20 UTC，默认渲染 10:08:35 和 23:59:35；固件使用 P4 内部 RTC + NTP。
- preview 把 `tick()` 一次性推进到未来 5 秒和 6 秒；固件按 33 ms 周期逐帧调用。
- preview 截 `lv_screen_active()`；固件也截整屏，但运行在真实面板上。
- 预览不覆盖旋转（P4 无 IMU）、WiFi 射频、音频和真机亮度/刷新率。

## 注意

- 修改 `main/` 下主题或 `clock_settings_menu.cpp` 后重新执行 `run.sh` 即可，无需刷机。
- 新增主题时，除 `main/main.cpp` 外，还要把主题注册到 `preview.cpp` 的 `kThemes`。
- 新增顶层字体需同时加入 `main/CMakeLists.txt` 和 `tools/preview/CMakeLists.txt`；M5 字体/图片在 preview 中由 GLOB 自动纳入，但设备端仍需显式添加。
- 视觉修改后先生成默认预览，再显式更新设备小预览：

  ```bash
  ./tools/preview/run.sh
  python3 tools/preview/generate_thumbnails.py
  ./tools/preview/run.sh
  ```

## 交互与提醒逻辑测试

```bash
cmake -S tools/preview -B tools/preview/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/preview/build -j8
ctest --test-dir tools/preview/build --output-on-failure
```

`alert_scheduler` 验证日期去重、并发排队、Snooze、校时和待补发倒计时；`menu_and_themes` 验证表盘选择、异步扫描、Sound 页、逐分钟编辑、午夜更新及主题反复创建/销毁。测试使用模拟扫描和主机内存，不验证射频、音频或真机动画帧率。
