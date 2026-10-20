# AI 工作指南

## 项目定位

这是一个 ESP32-S3 时钟项目，项目名为 `esp-clock`，目标硬件是 Waveshare ESP32-S3-Touch-AMOLED-1.75。

当前应用不是 Brookesia Phone Demo。不要按手机桌面、应用启动器或 Brookesia UI 的方向理解和修改本工程。以源码、构建配置和实际运行结果为准。

核心功能：

- 466 x 466 AMOLED 上显示可切换时钟主题。
- 使用 LVGL v9 构建 UI。
- 使用可复用 RGB565 draw buffer 和 `lv_snapshot_take_to_draw_buf()` 更新当前主题截图，再由 IMU 姿态驱动图片旋转。
- 使用 PCF85063 外部 RTC 同步系统时间。
- 使用 QMI8658 IMU 计算平面重力角度。
- GPIO0 单击切换时钟主题，长按打开设置菜单；触摸屏左右滑动手势切换主题，基本静止长按约 2 秒打开设置菜单。
- 设置菜单支持调节亮度、息屏超时、WiFi 开关，配置通过 NVS 持久化。
- 侧边电源键（IO 扩展器 EXIO4）单击用于手动息屏/亮屏。

## 关键模块

- `main/main.cpp`：应用入口、主题切换、LVGL snapshot、旋转弹簧、GPIO0 按钮/触摸手势/电源键接入、设置菜单、自动息屏。
- `main/clock_theme.h`：时钟主题接口。
- `main/clock_theme_minimal.cpp`、`main/clock_theme_segment.cpp`、`main/clock_theme_analog.cpp`、`main/clock_theme_outline_analog.cpp`、`main/clock_theme_mooncat.cpp`：本工程原有时钟主题。
- `main/clock_theme_m5_watch.cpp`：从 M5StopWatch-UserDemo 迁移的 M5 WatchFace 主题，包括 Classic、Simple、BigNumber、NumberFlow。
- `main/m5_assets/`、`main/m5_watch_assets.h`：M5 WatchFace 图片和字体资源。
- `main/rtc_time_sync.cpp`：PCF85063 RTC 同步，RTC 无效时使用固件编译时间兜底。
- `main/imu_demo.cpp`：QMI8658 加速度采样和时钟旋转角计算。
- `main/clock_settings.cpp` / `main/clock_settings.h`：NVS 设置读写（亮度、息屏超时、WiFi 开关）。
- `main/clock_settings_menu.cpp` / `main/clock_settings_menu.h`：LVGL 设置菜单 UI。
- `main/clock_touch_gesture.cpp` / `main/clock_touch_gesture.h`：触摸屏手势层（滑动切换主题、长按打开菜单、旋转坐标映射）。
- `components/qmi8658/`：QMI8658 I2C 驱动。
- `components/pcf85063/`：PCF85063 I2C 驱动。
- `components/attitude/`：保留的姿态估计算法，当前时钟应用不依赖、不启动。
- `components/ui_physics/`：角度弹簧平滑逻辑。
- `components/esp32_s3_touch_amoled_1_75/`：本地 Waveshare BSP。

## 常用命令

使用根目录脚本，不要直接假设本机 ESP-IDF 环境已经手动激活：

```bash
./dev.sh build
./dev.sh ports
./dev.sh flash-monitor
./dev.sh -p /dev/ttyUSB0 flash-monitor
```

构建成功时会生成 `build/esp-clock.bin`。当前工程使用自定义分区表，factory app 分区大小为 6 MB。

## AI 修改规则

- 修改前先读相关源码和 CMake 配置，不要只依据 README 判断当前工程状态。
- 优先沿用现有 ESP-IDF、LVGL、BSP 和组件风格。
- 保持改动范围小；不要做无关重构、目录重排或依赖替换。
- 不要删除字体、主题、BSP、本地驱动组件，除非用户明确要求。
- 不要把 `PROJECT_CONTEXT.md` 作为上下文来源；该文件已弃用。
- 默认使用中文与用户沟通，包括进度说明、最终总结、验证结果和 Git 提交信息；除非用户明确要求使用其他语言。
- Git 提交信息使用中文，除非用户明确要求使用其他语言。本工程提交信息前缀固定使用 `S3-clock: `，格式示例：`S3-clock: 优化表盘旋转跟手性`。
- 新增主题时实现 `clock_theme_t` 接口，并在 `main/main.cpp` 的主题数组中注册。
- 涉及 LVGL API 的代码必须在 LVGL 线程或显示锁保护下执行。
- IMU 任务不要直接调用 LVGL；通过共享目标角度或回调把数据交给 LVGL 定时器处理。

## 硬件注意事项

- GPIO0 单击切换主题、长按打开设置菜单；它也是 ESP32 启动相关脚，上电或复位时按住会影响启动模式。
- 触摸屏左右滑动切换主题，基本静止长按约 2 秒打开设置菜单；手势坐标会根据当前旋转角度做反向映射。
- 触摸累计移动超过 16 px 后会取消本次长按资格，避免慢滑误开设置菜单。
- PCF85063、QMI8658 和触摸控制器共用 BSP I2C 总线。
- `bsp_i2c_get_handle()` 会隐式初始化 I2C，RTC 可以在显示启动前使用。
- QMI8658 仅启用加速度计并读取 6 字节加速度数据；菜单打开或屏幕息屏时会暂停采样。
- 当前 UI 旋转方案复用一张 RGB565 snapshot，并且仅在主题内容实际变化时刷新。改动主题尺寸或刷新频率时要考虑 PSRAM 和碎片。
- 设置存在 NVS `clock_settings` 命名空间下，首次上电或 NVS 异常时会回退到默认值。

## 验证要求

- 代码改动后至少运行：

```bash
./dev.sh build
```

- 涉及串口、刷机或硬件交互时，先用 `./dev.sh ports` 确认端口。
- 如果没有硬件，最终说明中要明确哪些行为只完成了编译验证，哪些需要上板验证。
- 纯文档变更不需要构建，但需要检查路径、命令和模块描述是否与源码一致。
