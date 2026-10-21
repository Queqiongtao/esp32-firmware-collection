# AI 工作指南

## 项目定位

这是一个 ESP32-S3 时钟项目，项目名为 `esp-clock`，目标硬件是 Waveshare ESP32-S3-Touch-AMOLED-1.75。

当前应用不是 Brookesia Phone Demo。不要按手机桌面、应用启动器或 Brookesia UI 的方向理解和修改本工程。以源码、构建配置和实际运行结果为准。

核心功能：

- 466 x 466 AMOLED 上显示可切换时钟主题，当前主题索引通过 NVS 恢复。
- 使用 LVGL v9 构建 UI。
- 使用可复用 RGB565 draw buffer 和 `lv_snapshot_take_to_draw_buf()` 更新当前主题截图，再由 IMU 姿态驱动图片旋转。
- 使用 PCF85063 外部 RTC 同步系统时间。
- 使用 QMI8658 IMU 计算平面重力角度。
- GPIO0 单击切换时钟主题，长按打开设置菜单；触摸屏左右滑动手势切换主题，基本静止长按约 2 秒打开设置菜单。
- 主题切换带约 160 ms 的缩放淡出/淡入过渡；过渡期间的连续手势会累计步进，不丢切换。
- 设置菜单默认打开亮度页，支持调节亮度和息屏超时；点击菜单内容外区域或按 Done 关闭，屏幕息屏时也会自动关闭。WiFi 入口暂未实现，开关置灰并显示 `Not available`。
- 手动/自动息屏和亮屏使用约 200 ms 亮度渐变。
- 侧边电源键（IO 扩展器 EXIO4）单击用于手动息屏/亮屏。

## 关键模块

- `main/main.cpp`：应用入口、主题切换与过渡、LVGL snapshot、旋转弹簧、亮度渐变、GPIO0 按钮/触摸手势/电源键接入、设置菜单、自动息屏。
- `main/clock_theme.h`：时钟主题接口。
- `main/clock_theme_minimal.cpp`、`main/clock_theme_segment.cpp`、`main/clock_theme_analog.cpp`、`main/clock_theme_outline_analog.cpp`：本工程原有时钟主题。
- `main/clock_theme_m5_*.cpp`：从 M5StopWatch-UserDemo 迁移的 M5 WatchFace 主题，按 Classic、Simple、BigNumber、NumberFlow 分为四个文件。
- `main/lv_font_*.c`：主题位图字体资源；新增字体需同时加入 `main/CMakeLists.txt` 和 `tools/preview/CMakeLists.txt`。
- `main/m5_assets/`、`main/m5_watch_assets.h`：M5 WatchFace 图片和字体资源。
- `main/rtc_time_sync.cpp`：PCF85063 RTC 同步，RTC 无效时使用固件编译时间兜底。
- `main/imu_demo.cpp`：QMI8658 加速度采样和时钟旋转角计算。
- `main/clock_settings.cpp` / `main/clock_settings.h`：NVS 设置读写（亮度、息屏超时、WiFi 开关、主题索引）。
- `main/clock_settings_menu.cpp` / `main/clock_settings_menu.h`：LVGL 设置菜单 UI（亮度/息屏/WiFi 分页、点空白关闭）。
- `main/clock_touch_gesture.cpp` / `main/clock_touch_gesture.h`：触摸屏手势层（普通滑动/快速轻扫切换主题、长按打开菜单、旋转坐标映射）。
- `components/qmi8658/`：QMI8658 I2C 驱动。
- `components/pcf85063/`：PCF85063 I2C 驱动。
- `components/attitude/`：保留的姿态估计算法，当前时钟应用不依赖、不启动。
- `components/ui_physics/`：角度弹簧平滑逻辑。
- `components/esp32_s3_touch_amoled_1_75/`：本地 Waveshare BSP。
- `tools/preview/`：主机端主题预览工具，用 LVGL 桌面构建渲染主题 PNG，不参与 ESP-IDF 构建。
- `doc/theme-optimization-plan.md`：主题优化路线、进度和逐主题设计稿。

## 常用命令

使用根目录脚本，不要直接假设本机 ESP-IDF 环境已经手动激活：

```bash
./dev.sh build
./dev.sh ports
./dev.sh flash-monitor
./dev.sh -p /dev/ttyUSB0 flash-monitor
```

只调整主题视觉时，可以先在主机端出图自查，不必刷机：

```bash
./tools/preview/run.sh
```

产物在 `tools/preview/out/`，会同时输出圆屏遮罩图和越界报告。

构建成功时会生成 `build/esp-clock.bin`。当前工程使用自定义分区表，factory app 分区大小为 6 MB。

## AI 修改规则

- 修改前先读相关源码和 CMake 配置，不要只依据 README 判断当前工程状态。
- 优先沿用现有 ESP-IDF、LVGL、BSP 和组件风格。
- 保持改动范围小；不要做无关重构、目录重排或依赖替换。
- 不要删除字体、主题、BSP、本地驱动组件，除非用户明确要求。
- 不要把 `PROJECT_CONTEXT.md` 作为上下文来源；该文件已弃用。
- 默认使用中文与用户沟通，包括进度说明、最终总结、验证结果和 Git 提交信息；除非用户明确要求使用其他语言。
- Git 提交信息使用中文，除非用户明确要求使用其他语言。本工程提交信息前缀固定使用 `S3-clock: `，格式示例：`S3-clock: 优化表盘旋转跟手性`。
- 未经用户明确要求，不要执行 `git commit` 或 `git push`；只完成代码修改、构建验证和结果汇报，等用户确认后再提交。
- 新增主题时实现 `clock_theme_t` 接口，并在 `main/main.cpp` 的主题数组中注册。
- 涉及 LVGL API 的代码必须在 LVGL 线程或显示锁保护下执行。
- IMU 任务不要直接调用 LVGL；通过共享目标角度或回调把数据交给 LVGL 定时器处理。

## 硬件注意事项

- GPIO0 单击切换主题、长按打开设置菜单；它也是 ESP32 启动相关脚，上电或复位时按住会影响启动模式。
- 触摸屏左右滑动切换主题：普通滑动至少 100 px；快速轻扫至少 50 px、总时长不超过 300 ms，且松手前 80 ms 内仍有移动；水平/垂直位移比需大于 2.0；两次切换冷却 150 ms。
- 基本静止长按约 2 秒打开设置菜单；触摸累计移动超过 16 px 后会取消本次长按资格，避免慢滑误开设置菜单。
- 设置菜单默认亮度页；点内容外黑色区域关闭；息屏时菜单自动关闭，用于唤醒的触摸/按键不会同时切换主题或触发菜单项。
- WiFi 入口目前固定显示 `Not available`，开关禁用；`wifi_enabled` 设置字段保留，暂不参与连接。
- PCF85063、QMI8658 和触摸控制器共用 BSP I2C 总线。
- `bsp_i2c_get_handle()` 会隐式初始化 I2C，RTC 可以在显示启动前使用。
- QMI8658 仅启用加速度计并读取 6 字节加速度数据；菜单打开或屏幕息屏时会暂停采样。
- 当前 UI 旋转方案复用一张 RGB565 snapshot，并且仅在主题内容实际变化时刷新；主题切换过渡只改图片 scale/opacity，不额外分配 PSRAM。
- 设置存在 NVS `clock_settings` 命名空间下，首次上电或 NVS 异常时会回退到默认值；主题索引存于 `theme_id`，值是 `s_themes` 数组下标，增删或重排主题会改变映射。

## 验证要求

- 代码改动后至少运行：

```bash
./dev.sh build
```

- 涉及串口、刷机或硬件交互时，先用 `./dev.sh ports` 确认端口。
- 只调整主题视觉时，先跑 `./tools/preview/run.sh` 检查构图、圆屏裁切和越界报告，再上板确认观感。
- 交互改动建议上板检查：
  - 断电重启后是否恢复上次主题。
  - 连续快速滑动是否不丢步、不误开设置菜单；快速轻扫是否能触发切换。
  - 8 个主题的切换过渡是否正常；旋转过程中切换是否不残留旧主题。
  - 菜单是否默认亮度页、点内容外关闭、息屏时自动关闭；WiFi 页是否显示 `Not available` 且不可操作。
  - 手动/自动息屏和亮屏渐变是否正常；用于唤醒的触摸/按键是否不会同时触发切换或菜单。
- 如果没有硬件，最终说明中要明确哪些行为只完成了编译验证，哪些需要上板验证。
- 纯文档变更不需要构建，但需要检查路径、命令和模块描述是否与源码一致。
