# AI 工作指南

## 项目定位

这是一个 ESP32-S3 时钟项目，项目名为 `esp-clock`，目标硬件是 Waveshare ESP32-S3-Touch-AMOLED-1.75。

当前应用不是 Brookesia Phone Demo。不要按手机桌面、应用启动器或 Brookesia UI 的方向理解和修改本工程。以源码、构建配置和实际运行结果为准。

核心功能：

- 466 x 466 AMOLED 上显示可切换时钟主题。
- 使用 LVGL v9 构建 UI。
- 使用 `lv_snapshot_take()` 将当前主题截图为图片，再由 IMU 姿态驱动图片旋转。
- 使用 PCF85063 外部 RTC 同步系统时间。
- 使用 QMI8658 IMU 计算平面重力角度。
- GPIO0 单击切换时钟主题，快速甩腕也可切换主题。

## 关键模块

- `main/main.cpp`：应用入口、主题切换、LVGL snapshot、旋转弹簧、按钮和 IMU 手势接入。
- `main/clock_theme.h`：时钟主题接口。
- `main/clock_theme_minimal.cpp`、`main/clock_theme_segment.cpp`、`main/clock_theme_analog.cpp`、`main/clock_theme_outline_analog.cpp`、`main/clock_theme_mooncat.cpp`：本工程原有时钟主题。
- `main/clock_theme_m5_watch.cpp`：从 M5StopWatch-UserDemo 迁移的 M5 WatchFace 主题，包括 Classic、Simple、BigNumber、NumberFlow。
- `main/m5_assets/`、`main/m5_watch_assets.h`：M5 WatchFace 图片和字体资源。
- `main/rtc_time_sync.cpp`：PCF85063 RTC 同步，RTC 无效时使用固件编译时间兜底。
- `main/imu_demo.cpp`：QMI8658 采样、陀螺零偏校准、时钟旋转角计算、甩腕手势检测。
- `components/qmi8658/`：QMI8658 I2C 驱动。
- `components/pcf85063/`：PCF85063 I2C 驱动。
- `components/attitude/`：姿态估计算法。
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

- GPIO0 是当前主题切换按钮，也是 ESP32 启动相关脚。运行时可用，但上电或复位时按住会影响启动模式。
- PCF85063、QMI8658 和触摸控制器共用 BSP I2C 总线。
- `bsp_i2c_get_handle()` 会隐式初始化 I2C，RTC 可以在显示启动前使用。
- QMI8658 启动后会采样 200 次做陀螺零偏校准，校准期间应保持板子静止。
- 当前 UI 旋转方案会持有一张 RGB565 snapshot，刷新时短时间内可能同时存在新旧两张 snapshot。改动主题尺寸或刷新频率时要考虑 PSRAM 和碎片。

## 验证要求

- 代码改动后至少运行：

```bash
./dev.sh build
```

- 涉及串口、刷机或硬件交互时，先用 `./dev.sh ports` 确认端口。
- 如果没有硬件，最终说明中要明确哪些行为只完成了编译验证，哪些需要上板验证。
- 纯文档变更不需要构建，但需要检查路径、命令和模块描述是否与源码一致。
