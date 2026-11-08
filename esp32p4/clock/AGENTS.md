# AI 工作指南

## 项目定位

这是 ESP32-P4 800×800 圆屏时钟工程，目标硬件是 Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C。工程从 `esp32s3/clock` 移植，但按 P4 板卡重新适配，不是 S3 工程的共享构建目标。

核心功能：

- 800×800 原生圆屏 UI，8 个可切换时钟主题，主题索引存 NVS。
- 466×466 设计网格通过 `clock_theme_px()` 在编译期换算到 800；不要重新引入父容器 `transform_scale`。
- 设置菜单 4 页：Display、Power、WiFi、Sound；无 Motion 页、无电池卡片、无自动旋转。
- P4 无 IMU、无 AXP2101、无电池管理、无独立 PWR 键；BOOT 键为 GPIO35。
- WiFi 使用板载 ESP32-C6 + ESP-Hosted SDIO；启动时连接 C6 并打印 STA MAC。
- 时间由 P4 内部 RTC 保持，NTP 联网校正；未装 RTC 电池时用编译时间兜底。
- 音频使用 ES8311 + PA GPIO53，支持 UI 音效、整点报时、3 个闹钟、Timer 和睡眠白噪音。

## 关键模块

- `main/main.cpp`：应用入口、800×800 画布、主题切换/过渡、菜单、WiFi/NTP 状态、音频事件、ESP-Hosted 初始化、GPIO35 按键。
- `main/clock_theme.h`：主题接口、800/466 换算函数 `clock_theme_px()` / `clock_theme_pxf()`。
- `main/clock_theme_*.cpp`：8 个主题。M5 手型/数字图片按 466 生成，运行时 `lv_image_set_scale(439)` 放大；文字和点阵按 800 重新生成。
- `main/clock_settings_menu.cpp` / `.h`：4 页菜单、WiFi 配网/扫描/键盘、Sound/Alarm/Timer/Sleep 弹层。
- `main/clock_touch_gesture.cpp` / `.h`：触摸滑动/轻扫/长按；阈值按 800 放大。
- `main/wifi_manager.cpp` / `.h`：WiFi STA、NVS 凭据、AP 扫描、断线退避。
- `main/ntp_time_sync.cpp` / `.h`：SNTP 2 小时同步、失败退避、手动同步。
- `main/rtc_time_sync.cpp` / `.h`：P4 内部 RTC 与编译时间兜底。
- `main/audio_manager.cpp` / `.h`：ES8311/PA 音频。
- `main/clock_settings.cpp` / `.h`：NVS 设置；不包含 `auto_rotate`。
- `main/lv_font_*.c`、`main/m5_assets/`：800 布局字体与 M5 图片。
- `tools/preview/`：主机端 800×800 预览与交互测试。
- `tools/fonts/`：时钟/段码/M5 字体和点阵字形生成脚本。

## 常用命令

```bash
./dev.sh build
./dev.sh ports
./dev.sh flash-monitor -p /dev/ttyACM0
```

只调整主题或菜单视觉时：

```bash
./tools/preview/run.sh
ctest --test-dir tools/preview/build --output-on-failure
```

构建产物为 `build/esp-clock-p4.bin`，使用自定义分区表，factory app 分区 8 MB，partition table offset `0x10000`。

## AI 修改规则

- 修改前先读源码、CMake 和 `sdkconfig.defaults`，不要依据 S3 文档推断 P4 状态。
- 保持 800×800 原生布局；不要用 `transform_scale` 把 466 UI 放大。
- 新增/调整主题布局时使用 `clock_theme_px()` 包裹 466 设计值；字号按 800/466 比例重新生成，不要继续使用 S3 的 144/96/36 px 字体。
- 不重新引入 IMU、自动旋转、AXP2101、电池卡片、PWR/EXIO4 逻辑。
- 新增顶层字体需同时加入 `main/CMakeLists.txt` 和 `tools/preview/CMakeLists.txt`；M5 字体/图片在 preview 侧由 GLOB 自动纳入，设备端仍需显式添加。
- 新增主题时实现 `clock_theme_t`，并在 `main/main.cpp` 的 `s_themes` 和 `tools/preview/preview.cpp` 的 `kThemes` 同步注册。
- 涉及 LVGL API 的代码必须在 LVGL 线程或显示锁保护下执行。
- 默认使用中文沟通、进度说明和提交信息。Git 提交前缀固定为 `P4-clock: `。
- 未经用户明确要求，不执行 `git commit`、`git push` 或改写 Git 历史。

## 硬件注意事项

- 芯片：ESP32-P4 revision v1.0，360 MHz CPU，200 MHz HEX PSRAM；不要按 v3.0 的 400 MHz/250 MHz 配置构建。
- 本应用不使用 light sleep，`CONFIG_PM_SLEEP_CLK_ICG_ENABLE=n`；P4 v1.0 启动时默认的 sleep retention 初始化会因内存分配失败而 abort。
- 屏幕：800×800 IPS 圆屏，2-lane MIPI-DSI，80 MHz DPI。
- 触摸：GT9271/GT911 兼容，由 managed BSP `waveshare/esp32_p4_wifi6_touch_lcd_xc` 提供。
- WiFi：ESP32-C6，SDIO slot 1，GPIO14–19，reset GPIO54；Hosted 传输就绪前不要假设 `esp_wifi_init()` 可用。
- 音频：ES8311 + ES7210，PA GPIO53；LVGL task 栈固定 16 KB，Audio test 弹层控件较多，不要改回 8 KB。
- RTC：P4 内部 RTC + RTC 电池座；未装电池时冷启动时间丢失。
- 按键：BOOT GPIO35（低有效）、RESET；无 PWR 键。
- P4 无 IMU、无 AXP2101、无电池管理。

## 验证要求

- 代码改动后至少运行 `./dev.sh build`。
- 视觉/菜单改动先运行 `./tools/preview/run.sh` 和主机测试，检查圆屏裁切、菜单页面和越界报告。
- 有硬件时重点验证：显示方向与圆形裁切、触摸坐标、BOOT 键、ES8311 音频、C6 Hosted WiFi、NTP、P4 RTC 掉电保持、睡眠/唤醒亮度渐变。
- 上板验证后记录串口关键日志，并在最终说明中区分“已编译验证”“已上板验证”和“仍需硬件确认”。
