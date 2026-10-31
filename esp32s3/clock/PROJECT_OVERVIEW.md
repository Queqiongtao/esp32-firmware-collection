# esp-clock 工程说明

## 项目定位

`esp-clock` 是运行在 Waveshare ESP32-S3-Touch-AMOLED-1.75 上的圆屏时钟固件。它不是 Brookesia Phone Demo，也不是手机桌面或应用启动器项目；当前工程的核心目标是在 466 x 466 AMOLED 圆屏上显示可切换时钟主题，并可选地让整张时钟画面随设备姿态平滑旋转（默认关闭）。

工程使用 ESP-IDF 和 LVGL v9。UI 主题先按普通 LVGL 对象树构建，再通过 `lv_snapshot_take_to_draw_buf()` 复用 RGB565 draw buffer 完成截图；自动旋转开启时，最终由 IMU 姿态角驱动图片旋转。这样主题本身可以保持固定坐标系，旋转逻辑集中在主框架里处理。

## 当前功能

- 466 x 466 AMOLED 圆屏时钟显示。
- GPIO0 单击切换下一个时钟主题。
- 触摸屏左右滑动切换主题，长按打开设置菜单。
- 当前主题索引通过 NVS 恢复，重启后保持上次主题。
- 主题切换使用约 160 ms 的缩放淡出/淡入过渡；过渡期间的连续手势会累计步进。
- 设置菜单默认亮度页，支持亮度、息屏、自动旋转、WiFi 配网和 NTP 自动对时；按 Done 关闭，菜单内 1 分钟无操作自动关闭，点内容外区域不再关闭，息屏时自动关闭。
- WiFi 凭据可通过设置菜单的屏幕键盘输入，也可扫描附近 AP 后点选 SSID；凭据保存到 NVS，连接成功后自动启动 SNTP，对时结果写回 PCF85063。
- 手动/自动息屏和亮屏使用约 200 ms 亮度渐变；用于唤醒的触摸/按键不会同时执行切换或菜单操作。
- PCF85063 外部 RTC 同步系统时间。
- AXP2101 管理电池充电和电源路径，设置菜单 Power 页显示电池百分比、电压和充电状态。
- RTC 时间无效或不可用时，用固件编译时间兜底，并尝试写回 RTC。
- QMI8658 以 100 Hz 采样，计算屏幕平面内重力角度。
- 自动旋转默认关闭，可在设置菜单 Motion 页开启；关闭时时钟固定正向并暂停 IMU 采样。
- 自动旋转开启时，UI 旋转使用角度弹簧平滑，减少姿态变化时的抖动。
- 当前主题支持：
  - `minimal`（黑曜石极简）
  - `segment`（琥珀仪表 / Amber Deck）
  - `analog`（机械计时）
  - `outline_analog`（线框轨道）
  - `m5_classic`（经典机械）
  - `m5_simple`（薄荷珊瑚）
  - `m5_big_number`（冰火大数字）
  - `m5_number_flow`（数字流）

## 硬件平台

- 开发板：Waveshare ESP32-S3-Touch-AMOLED-1.75
- 主控：ESP32-S3
- Flash：16 MB
- PSRAM：8 MB
- 显示：466 x 466 AMOLED
- RTC：PCF85063
- IMU：QMI8658
- 触摸：CST9217
- 电源管理：AXP2101，支持 3.7 V 单节锂电池充电和电池监测
- WiFi：ESP32-S3 内置 2.4 GHz
- 主题切换按键：GPIO0，低电平有效

PCF85063、QMI8658 和触摸控制器共用 BSP I2C 总线。`bsp_i2c_get_handle()` 会隐式初始化 I2C，因此 RTC 可以在显示启动前同步系统时间。

## 运行框架

启动入口在 `main/main.cpp`。主要流程如下：

1. `app_main()` 设置时区为 `CST-8`，初始化 NVS 并读取 `clock_settings`（亮度、息屏超时、自动旋转、WiFi 开关、NTP 开关、主题索引）。
2. 校验主题索引；非法值回退到 `minimal`。
3. 调用 `rtc_time_sync_from_external_rtc()` 从 PCF85063 同步系统时间。
4. 初始化 AXP2101，关闭 TS 温度检测，配置充电参数并读取电池状态。
5. 初始化 WiFi 管理器和 SNTP；如果上次 WiFi 开关为开启且已有凭据，启动时自动连接。没有凭据时会把 `wifi_enabled` 回退为关闭。
6. 调用 `bsp_display_start()` 启动显示和 LVGL，并按保存的亮度初始化面板。
7. 在 LVGL 显示锁保护下调用 `create_clock_ui()` 创建当前主题和 snapshot 图片对象。
8. 注册 GPIO0 单击回调，用于切换主题。
9. 启动 IMU 任务，并根据 NVS 中的自动旋转设置决定是否采样；采样时接收加速度计姿态角回调。

主 UI 使用以下 LVGL timer：

- `update_clock()`：每 1000 ms 更新主题时间；仅在根主题内容变化且旋转已停稳时刷新 snapshot。
- `update_theme_animation()`：每 33 ms 调用当前主题的 `tick()`；覆盖层动画不触发 snapshot，根主题动画才按需刷新。
- `update_clock_rotation()`：自动旋转开启时，仅在旋转弹簧运动时以 16 ms 周期运行，停稳后自动暂停。
- `update_inputs()`：每 15 ms 处理按键、菜单切换和旋转定时器唤醒，并按 30 ms 周期轮询电源键。
- `update_theme_transition()`：16 ms 周期，仅在主题过渡期间运行；淡出 70 ms，中点切换主题并强制刷新 snapshot，再淡入 90 ms。
- `update_brightness_fade()`：16 ms 周期，仅在亮度渐变期间运行；息屏 220 ms、亮屏 240 ms。
- `update_network_status()`：每 500 ms 读取 WiFi 状态，按需启停 SNTP、把 NTP 同步结果写回 RTC，并刷新设置菜单中的网络状态。
- `update_power_status()`：每 2000 ms 读取 AXP2101 的电池电压、电量和充电状态，并刷新 Power 页。

菜单打开、屏幕息屏或自动旋转关闭时，时钟旋转和 IMU 采样会暂停；息屏还会关闭菜单、取消主题过渡并暂停渲染。恢复后立即同步当前时间与姿态。亮度渐变使用 LVGL timer 执行，避免与面板 QSPI IO 竞争。IMU 任务不会直接操作 LVGL，只设置共享目标角，并通知 LVGL timer 唤醒旋转处理。设置菜单固定在屏幕坐标系，不随设备姿态旋转。

## 主题切换方式

切换顺序由 `main/main.cpp` 中的 `s_themes` 数组决定，当前顺序为：

1. `minimal`
2. `segment`
3. `analog`
4. `outline_analog`
5. `m5_classic`
6. `m5_simple`
7. `m5_big_number`
8. `m5_number_flow`

支持的触发方式：

- GPIO0 单击：`clock_button_init()` 注册单击和长按回调，单击切换主题，长按打开或关闭设置菜单。
- 触摸左右滑动：根据当前屏幕旋转角反向映射触摸坐标，然后切换主题；静止长按约 2 秒打开设置菜单。普通滑动至少 100 px；快速轻扫至少 50 px、总时长不超过 300 ms，且松手前 80 ms 内仍有移动；水平/垂直位移比需大于 2.0；两次切换冷却 150 ms。

实际切换动作在 `update_inputs()` 的 LVGL timer 中启动。切换时 snapshot 图片先在 70 ms 内从 scale 256、opacity 255 过渡到 scale 244、opacity 0；中点销毁旧主题、创建新主题并强制刷新 snapshot；随后在 90 ms 内从 scale 244、opacity 0 回到 scale 256、opacity 255。过渡期间的新请求通过 `s_theme_switch_pending_step` 累计，快速连滑不会丢步。

主题切换完成后会把索引写入 NVS `clock_settings` 命名空间的 `theme_id` 键，并通过延迟定时器合并保存。该索引是 `s_themes` 数组下标；增删或重排主题会改变已有保存值的映射，需要同步迁移或接受重新映射。

需要注意：

- GPIO0 是 ESP32 启动相关脚，运行时可以作为按键使用，但上电或复位时按住可能进入异常启动模式。
- M5 原项目中 WatchFace 内部的点击切 palette、display mode 或秒点显示状态没有接入当前工程。
- 设置菜单使用独立的触摸对象；打开菜单时会禁用底层时钟手势层。
- 一次触摸累计移动超过 16 px 后会永久取消本次长按资格，但释放时仍按最终位移和方向判定是否切换主题。
- 主题过渡只改变 snapshot 图片的 scale 和 opacity，不新增整屏 draw buffer；如果缩放叠加旋转出现卡顿，可回退为仅透明度渐变。

## 设置菜单、息屏和亮度

设置菜单由 `main/clock_settings_menu.cpp` 创建，分页顺序为：

1. Brightness：胶囊进度条，范围 1%–100%，拖动时立即应用到面板并写入设置。
2. Power：可点击的电池卡片显示百分比、电压和充电/放电状态；点击进入 Battery 详情弹层，查看大号电量、USB 状态和充电配置。下方保留息屏超时选项。
3. Motion：Auto Rotate 开关，默认关闭；关闭时时钟固定正向并暂停 IMU 采样。
4. WiFi：WiFi 开关、Setup 配网按钮、Auto NTP 开关、Sync now 按钮和两行状态信息。连接后状态行显示 SSID，详情行显示 IP、RSSI 和 NTP 状态；失败时详情行显示 Network not found、Wrong password 等原因。
   - 点击 Setup 打开配网摘要页，显示 WiFi Name 和 Password 两个大条目，并提供 Scan networks 按钮扫描附近 AP；点选 SSID 后直接进入密码编辑，隐藏网络仍可手动输入。
   - 单字段编辑页使用约 350 × 270 的 6 列字母键盘，单键约 58 × 54 px；支持按键气泡、大小写和数字/符号层，输入框使用薄荷绿焦点边框，密码页支持显示/隐藏。
   - 摘要页的 Connect 保存凭据并开始连接，保存失败时保持弹层并显示错误；Cancel 关闭弹层。编辑页的 Done/Back 都会保留当前输入并返回摘要。配网弹层打开期间暂停菜单的 1 分钟自动关闭计时。

菜单默认停在 Brightness 页；按顶部中央 Done 关闭，菜单内 1 分钟无操作会自动关闭；点击菜单内容外的黑色区域不再关闭。屏幕息屏时菜单会自动关闭，避免唤醒后首触误操作。WiFi 配网弹层打开时不会触发菜单超时关闭。菜单固定在屏幕坐标系，不跟随设备姿态旋转。菜单打开时内容淡入缩放，关闭时内容先淡出、再短暂淡出黑色背景露出表盘；4 个图标 + 文字 Tab 沿底部浅弧排列，可直接切换 Display/Power/Motion/WiFi。菜单使用圆屏安全布局、纯黑背景；底部当前 Tab 为桃红，页面图标和亮度进度为薄荷绿，Done/选择态为琥珀色，深紫仅用于轨道和细边框。

息屏入口包括：

- 侧边电源键单击手动息屏/亮屏。
- 无操作超过设置中的超时时间后自动息屏。
- 触摸、GPIO0 或电源键可唤醒；用于唤醒的触摸或按键不会同时切换主题或触发菜单项。

息屏和亮屏的亮度变化由 `update_brightness_fade()` 在 LVGL timer 中执行：息屏约 220 ms 淡出到 0，亮屏约 240 ms 淡入到保存的亮度。息屏时同时取消主题过渡、关闭菜单并暂停时钟渲染和 IMU 采样；设置保存仍可继续完成。亮度渐变必须留在 LVGL timer 中，避免与面板 QSPI IO 竞争。

## 主题系统

所有主题都实现 `clock_theme_t` 接口：

```c
typedef struct {
    const char *name;
    lv_obj_t *(*build)(lv_obj_t *parent);
    bool (*update)(const struct tm *timeinfo);
    void (*reset)(void);
    void (*destroy_overlay)(void);
    bool (*tick)(uint32_t now_ms);
    bool (*tick_updates_overlay_only)(void);
    void (*set_overlay_visible)(bool visible);
    void (*set_rotation)(float angle_deg);
} clock_theme_t;
```

- `build()` 在父对象下创建 466 x 466 的主题根容器，并返回根对象。
- `update()` 根据当前 `struct tm` 更新时间、日期、指针或数字，并返回根主题内容是否实际变化。
- `reset()` 在主题进入时重置内部状态。
- `destroy_overlay()` 删除不属于主题根对象的覆盖层，例如 M5 Simple 秒点。
- `tick()` 用于主题内部动画，例如 M5 Simple 秒点移动/弹跳和 M5 NumberFlow 数字滚动。

主题对象树平时会被隐藏。主框架复用一张 466 x 466 RGB565 draw buffer，在刷新时临时显示根主题、隐藏 snapshot 图片和覆盖层，调用 `lv_snapshot_take_to_draw_buf()`，随后恢复显示层级。旋转过程中产生的时间变化会延迟到弹簧停稳后再截图。

新增主题时需要：

1. 实现 `clock_theme_t`。
2. 在 `main/clock_theme.h` 声明外部主题对象。
3. 在 `main/main.cpp` 的 `s_themes` 数组中注册。
4. 如有新资源，在 `main/CMakeLists.txt` 中加入源文件。

### 主题与菜单预览工具

`tools/preview/` 是主机端离屏预览工具，使用 LVGL 官方桌面构建把主题和设置菜单渲染成 PNG，不参与 ESP-IDF 构建。适合快速检查构图、对齐、圆屏裁切和配色，避免每改一次主题或菜单都刷机：

```bash
./tools/preview/run.sh
./tools/preview/run.sh --time 23:59:59
```

预览和固件编译的是同一批 `main/clock_theme_*.cpp`、`clock_theme.h`、`main/clock_settings_menu.cpp` 以及字体/图片资源，但它们不是同一个程序：preview 使用 `preview.cpp` 入口、固定日期 2026-10-20 UTC、虚拟 466 × 466 display 和独立的 `tools/preview/lv_conf.h`；固件使用 `main.cpp` 入口、PCF85063 RTC 时间、BSP AMOLED 和 `sdkconfig`，并包含旋转、主题过渡、设置菜单、触摸和 IMU 等运行时逻辑。

因此 preview 可以验证主题和菜单的构图、对齐、资源加载、圆屏裁切和配色，但不能验证真实面板的亮度/Gamma/色偏、旋转跟手性、主题过渡、菜单触摸交互、RTC 和长时间运行表现。预览通过不等于上板通过。

产物在 `tools/preview/out/`：默认看 `round/` 圆屏图，`raw/` 是未遮罩方图，`ppm/` 是主题原始渲染，`contact_native.png` 等是主题对比图，`menu/` 是圆屏遮罩后的设置菜单页面，`menu/raw/` 是方图，`menu/report.txt` 记录菜单内容的半径越界提示，`report.txt` 记录非黑像素范围与安全区越界告警。`report.txt` 中的“超出安全区”只是提示，不是渲染失败。

使用说明和产物详情见 `tools/preview/README.md`；与固件的完整差异、能力边界和维护约定见 `doc/preview-tool.md`。

主题优化路线、当前进度和各主题设计稿见 `doc/theme-optimization-plan.md`。

## M5 WatchFace 迁移

工程迁移了 `M5StopWatch-UserDemo` 中的四个 WatchFace：

- `Classic`（经典机械）
- `Simple`（薄荷珊瑚）
- `BigNumber`（冰火大数字）
- `NumberFlow`（数字流）

相关实现按主题拆分为：

- `main/clock_theme_m5_classic.cpp`
- `main/clock_theme_m5_simple.cpp`
- `main/clock_theme_m5_big_number.cpp`
- `main/clock_theme_m5_number_flow.cpp`

资源位于：

- `main/m5_assets/images/watch_face/classic/`
- `main/m5_assets/images/watch_face/big_number/`
- `main/m5_assets/fonts/`
- `main/m5_watch_assets.h`

迁移目标是默认进入主题后的显示效果、布局、颜色、字体和关键动画尽量与 M5 原项目一致。当前实现保留原始图片和字体资源，并在本工程 LVGL 主题接口下适配：

- `Classic`（经典机械）使用 canvas 绘制 466 x 466 刻度，并使用原始三根指针图片；本工程提高了星期对比度并调整日期胶囊尺寸。
- `Simple`（薄荷珊瑚）使用原始字体和 10 套主题色表；默认 C1 薄荷珊瑚配色，包含珊瑚大点、薄荷小点、薄荷日期胶囊和珊瑚糖果秒点，秒点平滑运动并在整秒轻微弹跳。
- `BigNumber`（冰火大数字）使用原始数字图片和 4 套 palette；本工程补充日期，并用上排冰蓝、下排珊瑚的冷暖分色区分小时与分钟。
- `NumberFlow`（数字流）在本工程内实现两位数字滚动效果，替代 M5 依赖库中的 `NumberFlow` 控件；使用 80px Commissioner 数字字体、H/M/S 标识和日期，秒位使用珊瑚色强调。

注意：M5 原项目中 WatchFace 内部触摸点击可切换 palette、display mode 或秒点显示状态。当前工程采用 snapshot 旋转架构，主题对象常态隐藏，触摸事件没有直接接入这些内部交互。本工程目前以默认显示效果一致为目标，主题切换由 GPIO0 或触摸滑动完成。

## 时间同步

时间同步有两条路径：开机时从 PCF85063 恢复时间，联网后通过 SNTP 校正并写回 RTC。

- 优先初始化 PCF85063，并读取 RTC 时间。
- 时间范围、日期、时分秒会做基本合法性检查。
- RTC 时间合法时写入系统时间。
- RTC 不可用或时间非法时解析 `__DATE__` 和 `__TIME__`，用固件编译时间初始化系统时间。
- 如果使用编译时间兜底，会尝试把编译时间写回 PCF85063。
- WiFi 连接成功后，`main/ntp_time_sync.cpp` 启动 SNTP，服务器依次为 `ntp.aliyun.com`、`ntp.tencent.com`、`cn.pool.ntp.org`；默认每小时自动更新时间。
- NTP 同步回调只记录同步时间和待写标志，真正的 PCF85063 写入由 `update_network_status()` 在 LVGL timer 中执行，避免阻塞 lwIP 任务。
- WiFi 页的 Sync now 可手动触发同步；即使 Auto NTP 关闭，手动同步也会保持 SNTP 运行最多 30 秒以完成本次对时。

系统时区在 `app_main()` 中设置为 `CST-8`。

## IMU 和旋转

IMU 逻辑位于 `main/imu_demo.cpp`。

- QMI8658 以 100 Hz 采样；自动旋转关闭、菜单打开或屏幕息屏时暂停采样，恢复后重新获取有效角度。
- 驱动只启用加速度计，并仅读取 6 字节加速度数据，不再执行启动陀螺校准或姿态积分。
- 使用加速度计的屏幕平面分量计算重力角度。
- 当水平重力分量过小时，说明设备接近平放，平面角度不可靠，会冻结最后有效角度。
- 对原始角度做一阶低通滤波。
- 自动旋转默认关闭，可在设置菜单 Motion 页开启；关闭时立即把时钟归正到 0°，并同步触摸坐标映射。
- 开启后第一帧有效角度直接作为目标，不需要先晃动设备。
- 旋转只应用到 snapshot 图片；设置菜单固定在屏幕坐标系。

旋转平滑逻辑在 `components/ui_physics/`。自动旋转开启时，主框架使用 `spring_angle_t` 将 IMU 目标角平滑成当前显示角，并把该角度应用到 snapshot 图片。

## 关键目录

- `main/main.cpp`：应用入口、主题注册与过渡、LVGL snapshot、旋转弹簧、亮度渐变、按钮和 IMU 回调接入。
- `main/clock_settings.cpp` / `main/clock_settings.h`：NVS 设置读写（亮度、息屏超时、自动旋转、WiFi 开关、NTP 开关、主题索引）。
- `main/clock_settings_menu.cpp` / `main/clock_settings_menu.h`：设置菜单 UI（亮度/息屏/自动旋转/WiFi、WiFi 配网摘要/扫描列表/编辑弹层和 NTP 控制）。
- `main/clock_touch_gesture.cpp` / `main/clock_touch_gesture.h`：触摸滑动/轻扫和长按手势层。
- `main/clock_theme.h`：主题接口。
- `main/clock_theme_*.cpp`：各时钟主题实现。
- `main/lv_font_*.c`：主题位图字体资源（如 `lv_font_clock_144.c`）。
- `main/clock_theme_m5_*.cpp`：M5 WatchFace 迁移主题。
- `main/m5_assets/`：M5 WatchFace 图片和字体资源。
- `main/rtc_time_sync.cpp`：PCF85063 RTC 同步，以及从系统时间写回 RTC。
- `main/wifi_manager.cpp` / `main/wifi_manager.h`：WiFi STA 连接、NVS 凭据、AP 扫描和断线重连。
- `main/ntp_time_sync.cpp` / `main/ntp_time_sync.h`：SNTP 启停、同步状态和手动对时。
- `main/power_manager.cpp` / `main/power_manager.h`：AXP2101 初始化、充电配置和电池状态读取。
- `main/imu_demo.cpp`：QMI8658 加速度采样和时钟平面角计算。
- `components/qmi8658/`：QMI8658 I2C 驱动。
- `components/pcf85063/`：PCF85063 I2C 驱动。
- `components/ui_physics/`：角度弹簧。
- `components/esp32_s3_touch_amoled_1_75/`：本地 Waveshare BSP。
- `tools/preview/`：主机端主题与设置菜单预览工具，用 LVGL 桌面构建渲染 PNG。
- `doc/preview-tool.md`：预览工具的定位、与固件的差异、能力边界和维护约定。
- `doc/theme-optimization-plan.md`：主题优化路线、进度与设计稿。

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
- snapshot 正常路径复用一张 466 x 466 RGB565 draw buffer，约 424 KB；只有尺寸异常时才会回退到临时分配。主题过渡只改图片 scale/opacity，不额外分配整屏 buffer。
- M5 Classic canvas 刻度也会占用一张 466 x 466 RGB565 draw buffer。
- 主题根容器应保持 466 x 466，并避免在主题内部再做整屏旋转。
- 主题索引保存的是 `s_themes` 数组下标；增删或重排主题时需考虑 NVS 中 `theme_id` 的兼容性。
- 自动旋转保存于 NVS `clock_settings` 命名空间的 `auto_rotate` 键，缺省为关闭；旧固件升级后没有该键时首次启动保持关闭。
- 主题预览列表硬编码在 `tools/preview/preview.cpp` 的 `kThemes` 中；新增或重排主题时需同步 `main/main.cpp` 的 `s_themes`，否则预览和固件会不一致。
- WiFi 凭据保存在 NVS `wifi_creds` 命名空间的 `ssid` 和 `pass` 键中，当前为明文存储；设置菜单的 Setup 弹层可覆盖已有凭据，只修改 SSID 时会沿用已保存密码。ESP-IDF NVS 的命名空间和键名最长 15 个字符，`wifi_creds` 已按该限制命名。
- WiFi 开关存于 `clock_settings` 命名空间的 `wifi` 键，NTP 自动对时存于 `ntp` 键且缺省开启；旧固件升级后没有 `ntp` 键时首次启动为开启。
- SNTP 配置 3 台服务器：`ntp.aliyun.com`、`ntp.tencent.com`、`cn.pool.ntp.org`。lwIP 在超时、DNS 解析失败或收到 KoD 时会尝试下一台；`CONFIG_LWIP_SNTP_MAX_SERVERS=3`，自动更新间隔为 1 小时。
- WiFi 断线后每 5 秒重试；NTP 同步成功或手动同步后会把系统本地时间写回 PCF85063。
- AXP2101 配置为 50 mA 预充、400 mA 恒流、25 mA 截止、4.2 V 截止；TS 引脚温度检测关闭，电池低于约 5% 时由 AXP2101 低压保护关机。PWR 按键在 EXIO4 上高电平有效，开机时用于上电的那次释放会被忽略，避免开机后立即息屏。
- WiFi 页状态与 `wifi_manager` 状态保持一致：关闭时显示 `WiFi off`，不再因为存在历史凭据显示 `WiFi ready`；连接失败会带上常见原因（如密码错误、找不到网络）。
- `Scan networks` 在 LVGL 事件中同步调用 `esp_wifi_scan_start()`；扫描期间界面停在 `Scanning...`，完成后最多显示 8 个 AP。WiFi 原本关闭时会临时启动 STA，扫描后恢复关闭；隐藏网络仍可手动输入。
- 亮度渐变和主题过渡都使用 LVGL timer；不要改成 FreeRTOS timer，避免与面板 QSPI IO 竞争。
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
- 断电重启后是否恢复上次主题。
- 触摸左右滑动是否能切换主题；静止约 2 秒是否能打开设置菜单，移动超过 16 px 后是否不再触发长按。
- 连续快速滑动是否不丢步、不误开设置菜单；快速轻扫是否能触发切换。
- 8 个主题的切换过渡是否正常；旋转过程中切换是否残留旧主题。
- 菜单是否默认亮度页、Done 可关闭、点内容外不关闭、1 分钟无操作自动关闭、息屏时自动关闭；Motion 页的 Auto Rotate 是否默认关闭，关闭后时钟是否立即回正。
- WiFi 页状态是否与开关一致，连接后是否显示 IP/RSSI，失败时是否显示原因；Auto NTP 和 Sync now 是否正常。
- WiFi 配网摘要页是否显示当前 SSID/密码状态，Scan networks 是否能列出附近 AP；SSID/密码编辑页是否一次只编辑一个字段、6 列键盘是否便于触摸、按键气泡/大小写/符号层/焦点高亮/密码显示隐藏是否正常；同步成功后 PCF85063 时间是否更新，断电重启后是否按保存的开关和凭据自动连接。
- Brightness 胶囊进度条拖动是否跟手，拖动时页面是否不会误滚动。
- 菜单打开/关闭的淡入缩放动画是否流畅，关闭后是否没有残留遮罩或菜单对象。
- 底部沿浅弧排列的 4 个图标 + 文字 Tab 是否可以直接切换到对应页面，当前 Tab 高亮是否正确，顶部中央 Done 是否可点击关闭，菜单内容是否全部在圆屏内。
- 手动/自动息屏和亮屏渐变是否正常；用于唤醒的触摸/按键是否不会同时触发切换或菜单。
- Power 页电池卡片是否显示百分比、电压和充电状态，点击是否打开 Battery 详情弹层；仅电池长按 PWR 是否能开机且开机后不会立即息屏；USB/电池切换和充电是否正常。
- 屏幕姿态变化时，自动旋转开启后时钟 snapshot 是否平滑旋转；关闭后是否保持正向。
- 自动旋转开关重启后是否保持；旧 NVS 首次启动是否默认关闭。
- 自动旋转关闭时，触摸滑动切换主题的坐标映射是否正常。
- 所有主题是否能正常显示，没有文本重叠、黑屏或异常闪烁。
- 只调整主题视觉时，先运行 `./tools/preview/run.sh` 检查构图和圆屏裁切；预览通过不代表上板通过，亮度、Gamma、旋转和过渡仍需上板确认。
- M5 四个主题的默认布局、颜色、字体、图片和动画是否与原项目预期一致。
- Simple 秒点是否平滑运动并在整秒轻微弹跳，且弹跳过程不会触发整屏 snapshot。
- NumberFlow 秒位是否滚动变化，而不是静态跳变。
- 串口是否出现 snapshot 分配、NVS 读写或 I2C 错误。

纯文档变更不需要构建，但需要检查路径、命令、主题名称和模块描述是否与源码一致。
