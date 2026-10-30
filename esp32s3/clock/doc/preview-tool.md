# 主题与菜单预览工具设计与边界

## 1. 定位

`tools/preview/` 是 `esp-clock` 的主机端离屏预览工具。它使用 LVGL 桌面构建，把 `main/` 下的时钟主题和设置菜单渲染成图片，用于在电脑上快速检查构图、对齐、字体、图片资源、圆屏裁切和配色。

它**不参与 ESP-IDF 构建**，也不会烧录到设备。预览通过不代表固件在真实 AMOLED 上的显示效果已经通过，亮度、Gamma、色偏、旋转跟手性、主题过渡和刷新性能仍需上板确认。

使用说明见 [`tools/preview/README.md`](../tools/preview/README.md)。

## 2. 与固件的关系

### 2.1 相同点

- 使用同一批主题源码：`main/clock_theme_*.cpp`。
- 使用同一个主题接口：`main/clock_theme.h` 中的 `clock_theme_t`。
- 使用同一批字体和图片资源：`main/lv_font_*.c`、`main/m5_assets/`、`main/m5_watch_assets.h`。
- LVGL 来自同一个 `managed_components/lvgl__lvgl`，当前版本为 9.5.0。
- 都使用 RGB565 和 LVGL 软件渲染。
- 主题实际使用的 Montserrat 字号（12~28）在两侧都已开启。
- 主题源码中没有 `ESP_PLATFORM` 之类的平台条件编译分支，因此同一份绘制逻辑在两侧行为一致。

### 2.2 不同点

| 方面 | preview | ESP32 固件 |
|------|---------|-----------|
| 程序入口 | `preview.cpp` 的 `main()` | `main.cpp` 的 `app_main()` |
| 参与编译的代码 | 8 个主题、设置菜单、字体/图片资源和 LVGL | 还包括设置、触摸手势、RTC、IMU、菜单、主题过渡等 |
| 时间来源 | 固定日期 2026-10-20 UTC；默认 10:08:35 和 23:59:35 | PCF85063 RTC + 系统本地时间（`CST-8`），持续走时 |
| 主题调用方式 | 每个主题执行一次 `build → reset → update → tick(+5s) → tick(+6s)` 后截图 | `update()` 每 1000 ms 调用一次；`tick()` 每 33 ms 调用；仅在内容变化且旋转停稳时刷新截图 |
| LVGL 配置 | 独立的 `tools/preview/lv_conf.h`；`LV_OS_NONE`、单线程、CLIB、Montserrat 12~40 子集、未启用压缩字体 | `sdkconfig`；FreeRTOS、CLIB、Montserrat 8~44、压缩字体、`LV_FONT_FMT_TXT_LARGE`、对象样式缓存、2 个软件绘制单元等 |
| 显示设备 | 466 × 466 虚拟 display，flush 回调为空实现 | CO5300 AMOLED，经本地 BSP 和 `esp_lvgl_adapter` 输出到真实面板 |
| 截图对象 | `lv_screen_active()` 整屏 | 当前主题 root，优先用 `lv_snapshot_take_to_draw_buf()` 写入可复用 RGB565 draw buffer |
| 截图方式 | `lv_snapshot_take()`，每次临时分配 | 复用一张 466 × 466 RGB565 buffer；失败时才回退到 `lv_snapshot_take()` |
| 旋转和覆盖层 | 不调用 `set_rotation()` 和 `set_overlay_visible()`；只在结束时调用 `destroy_overlay()` | 自动旋转开启时由 IMU 姿态驱动 snapshot 旋转，并单独管理 overlay 显隐 |
| 主题切换 | 不执行固件的缩放淡出/淡入过渡 | 70 ms 淡出、中点切换主题并刷新截图、90 ms 淡入 |
| 构建环境 | 主机 CMake、Release、标准库内存 | ESP-IDF、Xtensa GCC、PSRAM/内部堆 |
| 运行环境 | 单线程，无 FreeRTOS | FreeRTOS，LVGL 在 LVGL 线程或显示锁保护下运行 |

### 2.3 这意味着什么

preview 可以可靠验证：

- 主题构图、元素位置和对齐。
- 字体和图片资源是否正确加载。
- 圆屏裁切和安全区是否越界。
- 配色和大致明暗关系。
- 设置菜单四个页面的布局、图标、Tab、Done 和配色。
- 主题代码是否能正常构建、返回有效的 root 对象。

preview 不能验证：

- 真实 AMOLED 的亮度、Gamma、色偏和像素排列。
- UI 旋转的跟手性、主题切换过渡和刷新性能。
- 自动旋转开关、关闭后的归正行为和触摸坐标映射。
- RTC 时间、时区、跨天/跨月和星期显示。
- 设置菜单的触摸交互、开合动画流畅度和息屏逻辑；菜单布局和配色可以通过预览图检查。
- 长时间运行和逐帧 `tick` 下的动画中间状态。
- 固件侧的 snapshot 复用、buffer 分配和显示锁行为。

### 2.4 两个容易忽略的差异

1. **截图对象不同**：preview 截的是整个 screen，因此 M5 Simple 挂在 screen 上的秒点覆盖层也会被截到；固件截的是主题 root，覆盖层由主框架单独管理。两者截图范围并不完全等价。
2. **tick 推进方式不同**：preview 直接把时间推进到未来 5 秒和 6 秒；固件按 33 ms 周期逐帧调用。对于按“当前时间减开始时间”计算进度的动画结果基本一致，但依赖逐帧累加状态的主题可能表现出不同中间状态。

## 3. 渲染流水线

### 3.1 构建

`tools/preview/CMakeLists.txt` 负责主机端构建：

1. 从 `../../managed_components/lvgl__lvgl` 引入 LVGL，如果目录不存在会直接报错，提示先运行一次 `./dev.sh build` 拉取组件。
2. 使用 `tools/preview/lv_conf.h` 作为主机端 LVGL 配置。
3. 显式编译 8 个主题源码、`main/clock_settings_menu.cpp` 和顶层字体。
4. 用 `file(GLOB ...)` 自动收集 `main/m5_assets/fonts/*.c` 和 `main/m5_assets/images/watch_face/*/*.c`。
5. 通过 `tools/preview/stubs/esp_err.h` 提供主机端 `esp_err_t` 占位定义。
6. 生成 `tools/preview/build/preview` 可执行文件。

### 3.2 渲染

`tools/preview/preview.cpp` 的流程：

1. `lv_init()`。
2. 创建 466 × 466、RGB565、`LV_DISPLAY_RENDER_MODE_DIRECT` 的虚拟 display，draw buffer 为 `466 * 466 * 2` 字节，flush 回调直接调用 `lv_display_flush_ready()`。
3. 使用 `UTC` 时区，固定日期为 2026-10-20。
4. 默认渲染两个时刻：`10:08:35` 和 `23:59:35`；传入 `--time HH:MM:SS` 时只渲染指定时刻。
5. 对每个主题执行 `build()`、`reset()`、`update()`，再把 `tick()` 推进到当前 tick 之后 5 秒和 6 秒，让入场动画落到稳定结束状态。
6. 调用 `lv_obj_update_layout()`，再对 `lv_screen_active()` 执行 `lv_snapshot_take(..., LV_COLOR_FORMAT_RGB565)`。
7. 把 RGB565 逐像素移位成 RGB888，写出 PPM 到 `out/ppm/`。
8. 调用 `destroy_overlay()` 并清理 screen，继续下一个主题。
9. 主题渲染完成后，用默认设置创建一次菜单，渲染 `brightness`、`sleep`、`motion_off`、`motion_on`、`wifi` 五个页面到 `out/menu_ppm/`。
10. 菜单页面切换通过 `clock_settings_menu_set_page()`，截图前等待打开动画结束。

### 3.3 后处理

`tools/preview/render.py` 负责：

- 把 `out/ppm/*.ppm` 转成 `out/raw/*.png`。
- 叠加圆形遮罩，输出 `out/round/*.png`：
  - 圆半径 233（466 / 2）。
  - 圆外填中灰 `(38, 38, 38)`，避免和黑色主题背景混在一起。
  - 圆边画 2 px 亮灰 `(122, 122, 122)`，模拟玻璃边缘。
- 生成对比图：原生主题 2 列、M5 主题 2 列、全部主题 4 列。
- 把 `out/menu_ppm/*.ppm` 转成 `out/menu/*.png`（圆屏遮罩）和 `out/menu/raw/*.png`（方图），并生成 `out/menu/contact_menu.png` 2 列对比图和 `out/menu/report.txt` 半径报告。
- 统计非黑像素的包围盒和最大半径，写入 `out/report.txt`：
  - 非黑判定为 `r + g + b > 24`。
  - 最大半径超过 233：`⚠ 超出圆屏`。
  - 最大半径超过安全半径 205：`⚠ 超出安全区`。
  - 告警只是构图提示，不是渲染失败。

对比图按 `--time` 过滤，默认取 `10:08:35` 对应的图片；因此手动多次渲染不同时刻后，运行 `render.py` 时也要传入匹配的 `--time`。

## 4. 产物

| 路径 | 说明 |
|------|------|
| `out/round/*.png` | 默认查看：圆形遮罩 + 玻璃边缘 |
| `out/raw/*.png` | 未遮罩方图，调试构图用 |
| `out/ppm/*.ppm` | 原始渲染，466 × 466 RGB565 转 RGB888 |
| `out/contact_native.png` | 4 个原生主题圆屏对比图 |
| `out/contact_m5.png` | 4 个 M5 主题圆屏对比图 |
| `out/contact_all.png` | 全部 8 个主题圆屏对比图 |
| `out/menu/*.png` | 设置菜单页面（圆屏遮罩） |
| `out/menu/raw/*.png` | 设置菜单未遮罩方图 |
| `out/menu/contact_menu.png` | 5 个菜单页面对比图 |
| `out/menu/report.txt` | 菜单内容最大半径与越界提示 |
| `out/report.txt` | 非黑像素范围、最大半径和越界提示 |

`run.sh` 每次运行前会清空上一轮的 `out/ppm`、`out/raw`、`out/round`、`out/menu`、`out/menu_ppm`、`out/r1.0`、`out/*.ppm`、对比图和 `report.txt`，避免不同时刻的旧图混入对比图。

`out/menu/*.png` 的圆外灰色只表示真实圆屏之外的区域，不是菜单背景；纯黑方图在 `out/menu/raw/`。菜单半径报告在 `out/menu/report.txt`。

## 5. 维护约定

### 5.1 新增或重排主题

1. 在 `main/clock_theme.h` 声明新的主题对象。
2. 在 `main/clock_theme_*.cpp` 实现 `clock_theme_t`。
3. 在 `main/main.cpp` 的 `s_themes` 中注册。
4. 在 `tools/preview/preview.cpp` 的 `kThemes` 中按相同顺序注册。
5. 如有新的顶层字体或资源，加入 `main/CMakeLists.txt` 和 `tools/preview/CMakeLists.txt`。

`kThemes` 和 `s_themes` 是两份独立列表，没有自动同步检查；只改一侧会导致预览与固件主题数量或顺序不一致。重排主题还会改变 NVS 中 `theme_id` 的含义，需要评估兼容性。

### 5.2 字体和图片

- 顶层字体（`main/lv_font_*.c`）：两侧 `CMakeLists.txt` 都是显式列表，必须同时添加。
- M5 字体（`main/m5_assets/fonts/*.c`）：preview 用 GLOB 自动纳入，设备端 `main/CMakeLists.txt` 仍是显式列表。
- M5 图片（`main/m5_assets/images/watch_face/*/*.c`）：preview 用 GLOB 自动纳入，设备端仍需显式加入。
- 新增 Montserrat 字号时，还要确认两侧的 LVGL 配置都开启了对应字号；preview 目前只开启 12~40 的子集。

### 5.3 运行依赖

- 首次运行前至少执行一次 `./dev.sh build`，确保 `managed_components/lvgl__lvgl` 已下载。
- 需要 CMake、支持 C++17 的主机编译器、Python 3。
- `render.py` 依赖 Pillow（`PIL`），仓库没有提供 `requirements.txt`，需要自行安装，例如 `python3 -m pip install Pillow`。

### 5.4 使用限制

- `run.sh` 适合一次生成一个时刻的完整产物；要比较多个时刻，可以手动多次执行 `build/preview --time HH:MM:SS`，再运行一次 `render.py`。
- 预览固定日期，不会覆盖跨天、星期变化、月份边界或 12/24 小时制差异。
- 预览只验证构图和配色；亮度、Gamma、刷新流畅度、旋转和过渡必须上板确认。
- 设置菜单页面的布局、Tab、Done、配色和圆屏裁切可以预览，但触摸、开合动画流畅度和息屏逻辑仍要上板确认。
- `report.txt` 的“超出安全区”是提示，不是失败；当前多数主题会贴近边缘绘制外圈或边框。

## 6. 代码索引

| 文件 | 职责 |
|------|------|
| `tools/preview/run.sh` | 一键构建、清理产物、运行 preview、调用 render.py |
| `tools/preview/CMakeLists.txt` | 主机端 LVGL、主题和设置菜单构建配置 |
| `tools/preview/preview.cpp` | 虚拟 display、主题/菜单遍历、固定时间、截图和 PPM 输出 |
| `tools/preview/lv_conf.h` | 预览专用 LVGL 配置，尽量对齐设备端关键项 |
| `tools/preview/render.py` | PPM 转 PNG、主题和菜单圆屏遮罩、菜单方图/报告、对比图和越界报告 |
| `tools/preview/stubs/esp_err.h` | 主机端 `esp_err_t` 占位定义，仅供菜单源码编译 |
| `tools/preview/README.md` | 使用说明、产物说明和快速排错 |
