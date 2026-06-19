# esp-clock 工程说明

## 项目定位

`esp-clock` 是运行在 Waveshare ESP32-S3-Touch-AMOLED-1.75 上的圆屏时钟固件。它不是 Brookesia Phone Demo，也不是手机桌面或应用启动器项目；当前工程的核心目标是在 466 x 466 AMOLED 圆屏上显示可切换时钟主题，并让整张时钟画面随设备姿态平滑旋转。

工程使用 ESP-IDF 和 LVGL v9。UI 主题先按普通 LVGL 对象树构建，再通过 `lv_snapshot_take()` 截图成 RGB565 图片，最终由 IMU 姿态角驱动图片旋转。这样主题本身可以保持固定坐标系，旋转逻辑集中在主框架里处理。

## 当前功能

- 466 x 466 AMOLED 圆屏时钟显示。
- GPIO0 单击切换下一个时钟主题。
- QMI8658 快速甩腕手势切换下一个时钟主题。
- PCF85063 外部 RTC 同步系统时间。
- RTC 时间无效或不可用时，用固件编译时间兜底，并尝试写回 RTC。
- QMI8658 以 100 Hz 采样，计算屏幕平面内重力角度。
- UI 旋转使用角度弹簧平滑，减少姿态变化时的抖动。
- 当前主题支持：
  - `minimal`
  - `segment`
  - `analog`
  - `outline_analog`
  - `mooncat`
  - `m5_classic`
  - `m5_simple`
  - `m5_big_number`
  - `m5_number_flow`

## 硬件平台

- 开发板：Waveshare ESP32-S3-Touch-AMOLED-1.75
- 主控：ESP32-S3
- Flash：16 MB
- PSRAM：8 MB
- 显示：466 x 466 AMOLED
- RTC：PCF85063
- IMU：QMI8658
- 触摸：CST9217
- 主题切换按键：GPIO0，低电平有效

PCF85063、QMI8658 和触摸控制器共用 BSP I2C 总线。`bsp_i2c_get_handle()` 会隐式初始化 I2C，因此 RTC 可以在显示启动前同步系统时间。

## 运行框架

启动入口在 `main/main.cpp`。主要流程如下：

1. `app_main()` 设置时区为 `CST-8`。
2. 调用 `rtc_time_sync_from_external_rtc()` 从 PCF85063 同步系统时间。
3. 调用 `bsp_display_start()` 启动显示和 LVGL。
4. 在 LVGL 显示锁保护下调用 `create_clock_ui()` 创建当前主题和 snapshot 图片对象。
5. 注册 GPIO0 单击回调，用于切换主题。
6. 启动 IMU 任务，接收姿态角回调和甩腕手势回调。

主 UI 有三个 LVGL timer：

- `update_clock()`：每 1000 ms 更新当前主题的时间内容，并刷新 snapshot。
- `update_theme_animation()`：每 33 ms 调用当前主题的 `tick()`，用于主题内部动画；若动画有变化则刷新 snapshot。
- `update_clock_rotation()`：每 16 ms 更新旋转弹簧，并把角度应用到 snapshot 图片。

主题切换不会直接由 IMU 任务调用 LVGL。IMU 任务只设置共享的目标角度或切换请求，真正的 LVGL 对象删除、创建和刷新都在 LVGL timer 路径中执行。

## 主题切换方式

当前工程只有“切换到下一个主题”的运行时入口，不提供上一个主题、指定主题编号或触摸点选主题的入口。切换顺序由 `main/main.cpp` 中的 `s_themes` 数组决定，当前顺序为：

1. `minimal`
2. `segment`
3. `analog`
4. `outline_analog`
5. `mooncat`
6. `m5_classic`
7. `m5_simple`
8. `m5_big_number`
9. `m5_number_flow`

支持的触发方式：

- GPIO0 单击：`clock_button_init()` 使用 `iot_button_register_cb()` 注册 `BUTTON_SINGLE_CLICK`，回调只设置 `s_theme_switch_pending = true`。GPIO0 低电平有效。
- 快速甩腕：`imu_demo.cpp` 检测 QMI8658 的 Z 轴角速度，超过进入阈值后触发 `IMU_DEMO_GESTURE_THEME_NEXT`，并带有冷却时间和退出阈值，避免一次动作连续触发多次。

实际切换动作在 `update_clock_rotation()` 的 LVGL timer 中执行。该 timer 检查 `s_theme_switch_pending`，然后调用 `switch_to_next_theme()` 删除旧主题根对象、按顺序创建下一个主题、调用主题 `reset()`，最后立即 `update_clock()` 刷新 snapshot。

需要注意：

- GPIO0 是 ESP32 启动相关脚，运行时可以作为按键使用，但上电或复位时按住可能进入异常启动模式。
- 甩腕手势依赖 QMI8658 初始化和陀螺零偏校准；刚开机校准期间应保持设备静止。
- M5 原项目中 WatchFace 内部的点击切 palette、display mode 或秒点显示状态没有接入当前工程。当前工程的统一主题切换方式仍是 GPIO0 单击和甩腕手势。
- 当前触摸控制器虽然由 BSP 支持，但本工程没有把触摸事件用于主题切换或 M5 WatchFace 内部交互。

## 主题系统

所有主题都实现 `clock_theme_t` 接口：

```c
typedef struct {
    const char *name;
    lv_obj_t *(*build)(lv_obj_t *parent);
    void (*update)(const struct tm *timeinfo);
    void (*reset)(void);
    bool (*tick)(uint32_t now_ms);
} clock_theme_t;
```

- `build()` 在父对象下创建 466 x 466 的主题根容器，并返回根对象。
- `update()` 根据当前 `struct tm` 更新时间、日期、指针或数字。
- `reset()` 在主题进入时重置内部状态。
- `tick()` 用于主题内部动画，例如 M5 Simple 秒点动画和 M5 NumberFlow 数字滚动。

主题对象树平时会被隐藏，主框架在刷新 snapshot 时临时显示主题根对象、隐藏 snapshot 图片、调用 `lv_snapshot_take()`，随后再次隐藏主题根对象并显示 snapshot 图片。

新增主题时需要：

1. 实现 `clock_theme_t`。
2. 在 `main/clock_theme.h` 声明外部主题对象。
3. 在 `main/main.cpp` 的 `s_themes` 数组中注册。
4. 如有新资源，在 `main/CMakeLists.txt` 中加入源文件。

## M5 WatchFace 迁移

工程迁移了 `M5StopWatch-UserDemo` 中的四个 WatchFace：

- `Classic`
- `Simple`
- `BigNumber`
- `NumberFlow`

相关实现集中在 `main/clock_theme_m5_watch.cpp`，资源位于：

- `main/m5_assets/images/watch_face/classic/`
- `main/m5_assets/images/watch_face/big_number/`
- `main/m5_assets/fonts/`
- `main/m5_watch_assets.h`

迁移目标是默认进入主题后的显示效果、布局、颜色、字体和关键动画尽量与 M5 原项目一致。当前实现保留原始图片和字体资源，并在本工程 LVGL 主题接口下适配：

- `Classic` 使用 canvas 绘制 466 x 466 刻度，并使用原始三根指针图片。
- `Simple` 使用原始字体、10 套主题色、3 秒 tips 文案和秒点平滑运动。
- `BigNumber` 使用原始数字图片和 4 套 palette。
- `NumberFlow` 在本工程内实现两位数字滚动效果，替代 M5 依赖库中的 `NumberFlow` 控件。

注意：M5 原项目中 WatchFace 内部触摸点击可切换 palette、display mode 或秒点显示状态。当前工程采用 snapshot 旋转架构，主题对象常态隐藏，触摸事件没有直接接入这些内部交互。本工程目前以默认显示效果一致为目标，主题切换仍由 GPIO0 或甩腕手势完成。

## 时间同步

时间同步实现在 `main/rtc_time_sync.cpp`。

- 优先初始化 PCF85063，并读取 RTC 时间。
- 时间范围、日期、时分秒会做基本合法性检查。
- RTC 时间合法时写入系统时间。
- RTC 不可用或时间非法时解析 `__DATE__` 和 `__TIME__`，用固件编译时间初始化系统时间。
- 如果使用编译时间兜底，会尝试把编译时间写回 PCF85063。

系统时区在 `app_main()` 中设置为 `CST-8`。

## IMU 和旋转

IMU 逻辑位于 `main/imu_demo.cpp`。

- QMI8658 以 100 Hz 采样。
- 启动后采样 200 次做陀螺零偏校准，校准期间应保持板子静止。
- 使用加速度计的屏幕平面分量计算重力角度。
- 当水平重力分量过小时，说明设备接近平放，平面角度不可靠，会冻结最后有效角度。
- 对原始角度做一阶低通滤波。
- 甩腕切主题使用 Z 轴角速度阈值检测，并带有冷却时间，避免连续误触发。

旋转平滑逻辑在 `components/ui_physics/`。主框架使用 `spring_angle_t` 将 IMU 目标角平滑成当前显示角，并把该角度应用到 snapshot 图片。

## 关键目录

- `main/main.cpp`：应用入口、主题注册、LVGL snapshot、旋转弹簧、按钮和 IMU 回调接入。
- `main/clock_theme.h`：主题接口。
- `main/clock_theme_*.cpp`：各时钟主题实现。
- `main/clock_theme_m5_watch.cpp`：M5 WatchFace 迁移主题。
- `main/m5_assets/`：M5 WatchFace 图片和字体资源。
- `main/rtc_time_sync.cpp`：PCF85063 RTC 同步。
- `main/imu_demo.cpp`：QMI8658 采样、姿态角计算、甩腕手势。
- `components/qmi8658/`：QMI8658 I2C 驱动。
- `components/pcf85063/`：PCF85063 I2C 驱动。
- `components/attitude/`：姿态估计算法。
- `components/ui_physics/`：角度弹簧。
- `components/esp32_s3_touch_amoled_1_75/`：本地 Waveshare BSP。

## 构建和烧录

使用根目录脚本，不要假设当前 shell 已经手动激活 ESP-IDF：

```bash
./dev.sh build
./dev.sh ports
./dev.sh flash-monitor
./dev.sh -p /dev/ttyUSB0 flash-monitor
```

构建成功后生成：

- 固件：`build/esp-clock.bin`
- 分区表：自定义 `partitions.csv`

当前 factory app 分区大小为 6 MB。M5 字体和图片资源加入后固件体积明显增加，因此分区从原来的 4 MB 调整为 6 MB。

## 注意事项

- GPIO0 同时是启动相关脚。运行时可用于主题切换，但上电或复位时按住可能影响启动模式。
- LVGL API 必须在 LVGL 线程或显示锁保护下调用。
- IMU FreeRTOS 任务不要直接调用 LVGL；只通过共享目标角度或回调把数据交给 LVGL timer 处理。
- snapshot 刷新时可能短时间同时持有新旧两张 RGB565 图，466 x 466 x 2 字节约 424 KB，改主题尺寸、刷新频率或资源占用时要考虑 PSRAM 和内存碎片。
- M5 Classic canvas 刻度也会占用一张 466 x 466 RGB565 draw buffer。
- 主题根容器应保持 466 x 466，并避免在主题内部再做整屏旋转。
- 如果修改 RTC、IMU 或触摸相关逻辑，要注意三者共用 BSP I2C 总线。
- `PROJECT_CONTEXT.md` 已弃用，不应作为当前工程事实来源。

## 验证清单

代码改动后至少运行：

```bash
./dev.sh build
```

涉及硬件行为时建议上板检查：

- 开机后时间是否来自 PCF85063；RTC 无效时是否使用编译时间。
- GPIO0 单击是否能切换主题。
- 快速甩腕是否能切换主题，且不会连续误触发。
- 屏幕姿态变化时，时钟 snapshot 是否平滑旋转。
- 所有主题是否能正常显示，没有文本重叠、黑屏或异常闪烁。
- M5 四个主题的默认布局、颜色、字体、图片和动画是否与原项目预期一致。
- Simple 秒点是否平滑运动，tips 是否约 3 秒后隐藏。
- NumberFlow 秒位是否滚动变化，而不是静态跳变。

纯文档变更不需要构建，但需要检查路径、命令、主题名称和模块描述是否与源码一致。
