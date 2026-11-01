# AI 工作指南

## 项目定位

这是一个 ESP32-S3 时钟项目，项目名为 `esp-clock`，目标硬件是 Waveshare ESP32-S3-Touch-AMOLED-1.75。

当前应用不是 Brookesia Phone Demo。不要按手机桌面、应用启动器或 Brookesia UI 的方向理解和修改本工程。以源码、构建配置和实际运行结果为准。

核心功能：

- 466 x 466 AMOLED 上显示可切换时钟主题，当前主题索引通过 NVS 恢复。
- 使用 LVGL v9 构建 UI。
- 使用可复用 RGB565 draw buffer 和 `lv_snapshot_take_to_draw_buf()` 更新当前主题截图；IMU 旋转默认关闭，可在设置菜单 Motion 页开启。
- 使用 PCF85063 外部 RTC 同步系统时间。
- 使用 AXP2101 管理电池充电和电源路径，并在 Power 页显示电量、电压和充电状态；E-Gauge 无效时按电池电压估算电量，并用 `~` 前缀标识。
- 使用 ES8311 编解码器和 GPIO46 功放播放代码合成的 UI 音效、唤醒/熄屏提示音、整点报时、闹钟、倒计时和睡眠白噪音；睡眠白噪音仅 USB 供电可用。
- 使用 QMI8658 IMU 计算平面重力角度。
- GPIO0 单击切换时钟主题，长按打开设置菜单；触摸屏左右滑动手势切换主题，基本静止长按约 2 秒打开设置菜单。
- 主题切换带约 160 ms 的缩放淡出/淡入过渡；过渡期间的连续手势会累计步进，不丢切换。
- 设置菜单默认打开亮度页，支持调节亮度、息屏超时、自动旋转、WiFi 凭据和 NTP 自动对时，以及声音、闹钟、倒计时和睡眠白噪音；按顶部中央 Done 关闭，菜单内 1 分钟无操作会自动关闭；点击菜单内容外区域不再关闭。屏幕息屏时也会自动关闭。WiFi 连接成功后自动启动 SNTP，并把同步结果写回 PCF85063。
- 手动/自动息屏和亮屏使用约 200 ms 亮度渐变。
- 侧边电源键单击用于手动息屏/亮屏：EXIO4 高电平表示按下；固件同时每 100 ms 轮询 AXP2101 PKEY 正沿中断，作为电池供电下 EXIO4/TCA9554 读取不稳定时的兜底。两路事件做去重，并忽略开机时用于上电的那次释放，避免开机后立即息屏。AXP2101 负责长按关机和电池供电路径。

## 关键模块

- `main/main.cpp`：应用入口、主题切换与过渡、LVGL snapshot、旋转弹簧、亮度渐变、GPIO0 按钮/触摸手势/电源键接入、设置菜单、WiFi/NTP 状态轮询、音频事件/闹钟/倒计时、自动息屏。
- `main/clock_theme.h`：时钟主题接口。
- `main/clock_theme_minimal.cpp`、`main/clock_theme_segment.cpp`、`main/clock_theme_analog.cpp`、`main/clock_theme_dot_matrix.cpp`：本工程原有时钟主题。
- `main/clock_theme_m5_*.cpp`：从 M5StopWatch-UserDemo 迁移的 M5 WatchFace 主题，按 Classic、Simple、BigNumber、NumberFlow 分为四个文件。
- `main/lv_font_*.c`：主题位图字体资源；新增字体需同时加入 `main/CMakeLists.txt` 和 `tools/preview/CMakeLists.txt`。
- `main/m5_assets/`、`main/m5_watch_assets.h`：M5 WatchFace 图片和字体资源；M5 字体已用原始 TTF 重新生成为 4bpp（见 `tools/fonts/README.md`），`m5_number_flow` 的 80px 数字和 28px 日期是当前设计，不是上游的 64/24。
- `main/rtc_time_sync.cpp`：PCF85063 RTC 同步，RTC 无效时使用固件编译时间兜底；NTP 同步后由主循环调用 `rtc_time_sync_write_from_system()` 写回 RTC。
- `main/imu_demo.cpp`：QMI8658 加速度采样和时钟旋转角计算。
- `main/wifi_manager.cpp` / `main/wifi_manager.h`：WiFi STA 连接状态机、NVS 凭据读写、AP 扫描和断线重连。
- `main/ntp_time_sync.cpp` / `main/ntp_time_sync.h`：SNTP 启停、手动触发同步和同步状态。
- `main/power_manager.cpp` / `main/power_manager.h`：AXP2101 初始化、充电配置、电池状态读取，以及 E-Gauge 无效时的电压估算兜底。
- `main/audio_manager.cpp` / `main/audio_manager.h`：ES8311/PA 初始化、音频任务、代码合成音效/闹钟/白噪音和音量控制。
- `main/clock_settings.cpp` / `main/clock_settings.h`：NVS 设置读写（亮度、息屏超时、自动旋转、WiFi 开关、NTP 开关、主题索引、声音/音量/报时和 3 个闹钟）。
- `main/clock_settings_menu.cpp` / `main/clock_settings_menu.h`：LVGL 设置菜单 UI（亮度/息屏/自动旋转/WiFi/Sound 分页、WiFi 配网摘要/扫描列表/单字段编辑弹层、Sound/Alarm/Timer/Sleep 弹层、NTP 控制和 Done/超时关闭）。
- `main/clock_touch_gesture.cpp` / `main/clock_touch_gesture.h`：触摸屏手势层（普通滑动/快速轻扫切换主题、长按打开菜单、旋转坐标映射）。
- `components/qmi8658/`：QMI8658 I2C 驱动。
- `components/pcf85063/`：PCF85063 I2C 驱动。
- `components/attitude/`：保留的姿态估计算法，当前时钟应用不依赖、不启动。
- `components/ui_physics/`：角度弹簧平滑逻辑。
- `components/esp32_s3_touch_amoled_1_75/`：本地 Waveshare BSP。
- `tools/preview/`：主机端预览工具，用同一批主题源码、设置菜单源码和 LVGL 桌面构建渲染 PNG，不参与 ESP-IDF 构建；运行环境与固件不同，详见 `doc/preview-tool.md`。
- `doc/preview-tool.md`：预览工具的定位、与固件的差异、能力边界和维护约定。
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
- 新增主题时实现 `clock_theme_t` 接口，并在 `main/main.cpp` 的主题数组和 `tools/preview/preview.cpp` 的 `kThemes` 中同步注册；两处列表不会自动同步。新增顶层字体需同时加入 `main/CMakeLists.txt` 和 `tools/preview/CMakeLists.txt`，M5 字体/图片在预览侧由 GLOB 自动纳入，但设备端仍需显式添加。
- 涉及 LVGL API 的代码必须在 LVGL 线程或显示锁保护下执行。
- IMU 任务不要直接调用 LVGL；通过共享目标角度或回调把数据交给 LVGL 定时器处理。

## 硬件注意事项

- GPIO0 单击切换主题、长按打开设置菜单；它也是 ESP32 启动相关脚，上电或复位时按住会影响启动模式。
- 触摸屏左右滑动切换主题：普通滑动至少 100 px；快速轻扫至少 50 px、总时长不超过 300 ms，且松手前 80 ms 内仍有移动；水平/垂直位移比需大于 2.0；两次切换冷却 150 ms。
- 基本静止长按约 2 秒打开设置菜单；触摸累计移动超过 16 px 后会取消本次长按资格，避免慢滑误开设置菜单。
- 设置菜单默认亮度页，分页顺序为亮度、息屏、自动旋转、WiFi、Sound；触摸操作仅顶部中央 Done 可关闭，1 分钟无操作会自动关闭，点内容外黑色区域不再关闭；GPIO0 物理键和息屏仍可关闭。息屏时菜单自动关闭，用于唤醒的触摸/按键不会同时切换主题或触发菜单项。
- 自动旋转默认关闭；关闭时时钟固定正向、IMU 采样暂停，触摸手势使用未旋转的坐标映射。开启后按当前姿态立即旋转，不需要先晃动设备。
- WiFi 页提供开关、Setup 配网、Auto NTP 开关、Sync now 和两行状态显示；连接后显示 SSID、IP、RSSI 和 NTP 同步状态，失败时显示错误原因。保存凭据或启动连接失败时，配网弹层保持打开并显示错误，不再静默关闭。
- Setup 弹层采用“摘要页 + 单字段编辑页”：摘要页显示 WiFi Name/Password 两个大条目，并提供 Scan networks 按钮扫描附近 AP；点选 SSID 后直接进入密码编辑，隐藏网络仍可手动输入。编辑页只显示一个输入框，使用约 350 × 270 的 6 列字母键盘，单键约 58 × 54 px；支持按键气泡、大小写和数字/符号层，当前输入框用薄荷绿边框高亮，密码支持显示/隐藏。密码以明文保存在 NVS `wifi_creds` 命名空间；连接成功后 SNTP 自动启动，依次使用 `ntp.aliyun.com`、`ntp.tencent.com`、`cn.pool.ntp.org` 三台服务器。
- WiFi 扫描在 LVGL 事件中同步执行，扫描期间界面不响应触摸，通常持续 1–3 秒；最多展示 8 个 AP。WiFi 原本关闭时会临时启动 STA，扫描完成后关闭。
- AXP2101 使用共享 BSP I2C 总线，地址 `0x34`；初始化时关闭 TS 引脚温度检测，设置 50 mA 预充、400 mA 恒流、25 mA 截止、4.2 V 截止电压，并显式使能 E-Gauge（REG18h bit3）。本板 E-Gauge 寄存器 `0xA4` 可能持续返回 0，此时 `power_manager` 使用 3300–4200 mV 电压曲线估算电量（充电时先补偿 70 mV），模块状态中的 `battery_percent_estimated` 为 true，UI 用 `~` 前缀和 `Estimated | 4.2 V / 400 mA` 标识估算值；串口首次更新会打印 `gauge=... percent=... (estimated) battery_mv=...`。
- PCF85063、QMI8658、AXP2101、ES8311 和触摸控制器共用 BSP I2C 总线；ES8311 通过 I2S（MCLK42/BCLK9/WS45/DOUT8）输出，PA 使能为 GPIO46。
- 睡眠白噪音只在 USB 供电时允许启动；播放中拔掉 USB 会淡出并停止。闹钟允许电池供电，但 60 秒自动停止。白噪音播放期间 UI 音效会暂时静音，Test 试听会先停止白噪音。
- `bsp_i2c_get_handle()` 会隐式初始化 I2C，RTC 可以在显示启动前使用。
- QMI8658 仅启用加速度计并读取 6 字节加速度数据；菜单打开、屏幕息屏或自动旋转关闭时会暂停采样。
- 当前 UI 旋转方案复用一张 RGB565 snapshot，并且仅在主题内容实际变化时刷新；主题切换过渡只改图片 scale/opacity，不额外分配 PSRAM。
- 设置存在 NVS `clock_settings` 命名空间下，首次上电或 NVS 异常时会回退到默认值；主题索引存于 `theme_id`，值是 `s_themes` 数组下标，增删或重排主题会改变映射。声音设置存于 `sound_on`、`sound_vol`、`ui_sound`、`chime_on`，3 个闹钟以 `alarm_cfg` blob 保存。移除 outline 主题后，旧固件升级时若缺少 `theme_map` 键，会把旧索引 3 回退到 analog，并把旧索引 4–7 前移为 3–6，然后写入 `theme_map=1`。自动旋转存于 `auto_rotate`，缺省为关闭；NTP 自动对时存于 `ntp`，缺省为开启。WiFi 凭据单独存于 `wifi_creds` 命名空间，SSID 和密码均为明文。ESP-IDF NVS 的命名空间和键名最长 15 个字符，新增 NVS 名称时必须检查长度。

## 验证要求

- 代码改动后至少运行：

```bash
./dev.sh build
```

- 涉及串口、刷机或硬件交互时，先用 `./dev.sh ports` 确认端口。
- 只调整主题视觉或设置菜单时，先跑 `./tools/preview/run.sh` 检查构图、圆屏裁切、菜单页面和越界报告，再上板确认观感；`report.txt` 中的“超出安全区”是提示而非失败，预览通过也不等于上板通过（亮度、Gamma、旋转、过渡仍需上板确认）。
- 交互改动建议上板检查：
  - 断电重启后是否恢复上次主题。
  - 连续快速滑动是否不丢步、不误开设置菜单；快速轻扫是否能触发切换。
  - 8 个主题的切换过渡是否正常；旋转过程中切换是否不残留旧主题。
  - M5 主题字体是否为 4bpp，`m5_simple` 108px 和 `m5_number_flow` 80px 数字边缘是否清晰；重新生成字体后先跑预览，再上板确认，生成方法见 `tools/fonts/README.md`。
  - Sound 页音量、UI 音效、整点报时和 Audio test 弹层是否正常（白噪音播放时 UI 音效会暂时静音）；各音效音色是否可区分。休眠和熄屏是否有轻提示音，休眠后触摸/PWR 唤醒是否有轻提示音，唤醒后第一次切主题是否有声；点底部 Tab 是否只有一声、滑动切页是否只有一声；3 个闹钟的时间/星期/开关、响铃、Snooze/Stop 和亮屏是否正常；Timer/Pomodoro 和结束提示音是否正常；睡眠白噪音是否只在 USB 下启动、拔掉 USB 是否停止。
  - 菜单是否默认亮度页、Done 可关闭、点内容外不关闭、1 分钟无操作自动关闭、息屏时自动关闭；Motion 页开关是否默认关闭，关闭后时钟是否立即回正；WiFi 页状态是否与开关一致，能否显示 IP/RSSI/失败原因。
  - WiFi 配网摘要页是否显示当前 SSID/密码状态，Scan networks 是否能列出附近 AP 并在点选后进入密码页；SSID/密码编辑页是否一次只编辑一个字段、6 列键盘是否足够大、按键气泡/大小写/符号层/焦点/密码显示隐藏是否正常；保存凭据并连接、Auto NTP 和 Sync now 是否可用；串口不应再出现 `ESP_ERR_NVS_KEY_TOO_LONG`。
  - WiFi 连接成功后 NTP 是否自动同步，Sync now 是否能立即触发同步，同步后 PCF85063 时间是否更新；断电重启后 WiFi 是否按保存的开关和凭据自动连接。
  - Brightness 胶囊进度条拖动是否跟手且不会误触发页面滚动；菜单打开/关闭动画是否流畅、无残留菜单对象。
  - 底部沿浅弧排列的 5 个图标 + 文字 Tab 是否可以直接切换页面，当前 Tab 高亮是否正确；顶部中央 Done 是否可点击关闭，且所有控件都在圆屏内。
  - 自动旋转开启后，断电重启是否保持开启；关闭后是否保持关闭，且启动时钟不跟随倾斜。
  - 自动旋转关闭时，触摸滑动切换主题的坐标映射是否正常，没有残留旋转。
  - 手动/自动息屏和亮屏渐变是否正常；用于唤醒的触摸/按键是否不会同时触发切换或菜单。
  - Power 页电池卡片是否显示电池百分比、电压和充电/放电状态，点击后 Battery 详情弹层是否正常；E-Gauge 为 0 时是否显示 `~` 估算电量和 `Estimated` 提示，串口是否打印 `gauge=0 percent=... (estimated)`；仅电池长按 PWR 是否能开机且开机后不会立即息屏；仅电池供电时 PWR 单击息屏/唤醒是否稳定（AXP PKEY 兜底是否生效）；USB 与电池切换、充电状态是否正常。
- 如果没有硬件，最终说明中要明确哪些行为只完成了编译验证，哪些需要上板验证。
- 纯文档变更不需要构建，但需要检查路径、命令和模块描述是否与源码一致。
