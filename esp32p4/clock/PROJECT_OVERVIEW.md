# ESP32-P4 800×800 时钟工程说明

## 目标与边界

`esp32p4/clock` 是从 `esp32s3/clock` 移植的独立 ESP-IDF 工程，目标板为 Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C。它不是 S3 工程的共享构建目标，也不修改 S3 工程。

移植后的产品边界：

- 目标芯片为 ESP32-P4 revision v1.0：360 MHz CPU、200 MHz HEX PSRAM，使用 pre-v3 寄存器集。
- 800×800 圆屏，UI 使用原生 800×800 坐标和按 800 重新生成的字体。
- 无 IMU、无自动旋转、无 Motion 设置页。
- 无 AXP2101、无电池管理、无 PWR 键；Power 页只保留亮度和息屏超时。
- WiFi 由板载 ESP32-C6 通过 ESP-Hosted SDIO 提供。
- 时间由 P4 内部 RTC 保持，NTP 联网校正；RTC 电池未安装时冷启动使用编译时间兜底。
- 音频仍使用 ES8311/PA，引脚由 managed BSP 配置。

## 目录结构

```text
esp32p4/clock/
├── main/
│   ├── main.cpp                 应用入口、显示/触摸、主题切换、菜单、WiFi/NTP
│   ├── clock_theme.h            主题接口与 466→800 换算
│   ├── clock_theme_*.cpp        8 个主题
│   ├── clock_settings_menu.*    4 页设置菜单与弹层
│   ├── clock_touch_gesture.*    触摸手势层
│   ├── wifi_manager.*           WiFi STA、扫描、重连
│   ├── ntp_time_sync.*          SNTP
│   ├── rtc_time_sync.*          P4 内部 RTC 兜底
│   ├── audio_manager.*          ES8311/PA 音频
│   ├── clock_settings.*         NVS 设置
│   ├── lv_font_*.c              800 布局字体
│   └── m5_assets/               M5 主题字体和图片
├── tools/preview/               主机端 800×800 预览
├── tools/fonts/                 字体/点阵生成脚本
├── sdkconfig.defaults           800×800、32 MB Flash、PSRAM、Hosted 配置
├── partitions.csv               8 MB factory app
└── dev.sh                       eim 构建入口
```

## 显示与主题

### 800×800 原生布局

主题源码保留 S3 的 466×466 设计网格，所有位置、尺寸、线宽和字距通过 `clock_theme.h` 中的 `clock_theme_px()` / `clock_theme_pxf()` 在编译期换算：

```cpp
static constexpr int32_t CLOCK_THEME_SIZE = 800;
static constexpr int32_t CLOCK_THEME_DESIGN_SIZE = 466;
```

这样做的目的不是运行时缩放，而是让布局常量仍然可读，同时所有文字和矢量图形由 LVGL 在 800×800 上原生绘制。`main.cpp` 中的 `s_ui_root` 是普通 800×800 容器，不再设置 `transform_scale`。

主题切换仍复用一张 800×800 RGB565 snapshot：先隐藏 snapshot 图片，截取整屏，之后仅通过图片 scale/opacity 完成过渡。主题每秒更新时只在内容变化时刷新 snapshot。

LVGL 任务栈从 8 KB 提升到 16 KB。设置菜单的 Audio test 弹层会在一次事件回调中创建 11 个按钮和对应标签，8 KB 栈在设备上存在溢出风险。

### 主题列表

| 索引 | 名称 | 主要字体/资源 |
| --- | --- | --- |
| 0 | minimal | `lv_font_clock_248`、Montserrat 34 |
| 1 | segment | `lv_font_seg_165`、`lv_font_seg_62`、Montserrat 42 |
| 2 | analog | Montserrat 20/48、矢量表盘 |
| 3 | m5_classic | Maple Mono 48/41、手型图片 |
| 4 | m5_simple | Commissioner 185、Montserrat 42、圆点叠加层 |
| 5 | m5_big_number | 数字图片、Montserrat 42 |
| 6 | m5_number_flow | Commissioner 137、Montserrat 28/48 |
| 7 | dot_matrix | 15×25 点阵掩码、Montserrat 34/48 |

M5 手型和数字图片按 466 网格生成，运行时通过 `lv_image_set_scale(439)` 放大到 800；文字和点阵 canvas 均按 800 重新渲染。

## 设置菜单

设置菜单位于 `main/clock_settings_menu.cpp`，使用 4 页：

1. **Display**：亮度、Watch faces 两页缩略图选择器。
2. **Power**：USB 供电状态、1/5/15 分钟/关闭息屏。
3. **WiFi**：WiFi 开关、Setup、Auto NTP、Sync now、连接状态。
4. **Sound**：主开关、音量、UI 音效、整点报时、Audio test、Alarms、Timer、Sleep noise。

菜单根容器、页面和弹层均为 800×800，内部尺寸/字号通过 `clock_theme_px()` 换算。Motion 页和电池卡片已删除；`clock_settings_t` 不再包含 `auto_rotate`。

WiFi 配网沿用 S3 的“摘要页 + 单字段编辑页 + 6 列键盘 + 异步扫描”流程。扫描最多显示 8 个 AP，15 秒超时；扫描期间可以返回或关闭菜单。

## 网络与时间

### ESP-Hosted

`sdkconfig.defaults` 使用 `esp_wifi_remote` + `esp_hosted`，默认 SDIO slot 1、GPIO14–19、reset GPIO54，与 3.4C 板卡一致。

`app_main()` 在 WiFi 初始化前调用 `p4_network_init()`：

1. 创建默认事件循环。
2. 注册 `ESP_HOSTED_EVENT`，等待 `ESP_HOSTED_EVENT_TRANSPORT_UP`，超时 10 秒。
3. 调用 `esp_hosted_connect_to_slave()`。
4. 传输就绪后由 `wifi_manager_init()` 执行 `esp_wifi_init()`，随后打印 `esp_wifi_get_mac(WIFI_IF_STA)`。

连接失败只记录警告，固件继续启动；时钟、显示和音频不依赖网络。

### NTP 与 RTC

`ntp_time_sync.cpp` 使用 `ntp.aliyun.com`、`ntp.tencent.com`、`cn.pool.ntp.org`，正常同步间隔 2 小时。失败后退避 5/15/30/60 分钟。同步成功后系统时间由 P4 内部 RTC 域保持。

`rtc_time_sync_from_external_rtc()` 在 P4 上：

- 若系统时间已合理（2024–2099），保留 RTC 时间。
- 否则解析固件编译时间作为兜底。
- `rtc_time_sync_write_from_system()` 为空操作，因为系统时间本身就在 RTC 域中。

## 音频

`audio_manager.cpp` 从 S3 移植，使用 BSP 的 `bsp_audio_codec_speaker_init()`、ES8311 和 PA。支持：

- UI 点击/确认/错误/滑动/菜单打开音效
- 唤醒/熄屏提示音
- 整点报时
- 3 个闹钟（Chime/Beep/Melody）与 5 分钟 Snooze
- Timer/Pomodoro 结束提示
- 15/30/60 分钟白/棕噪音（USB 供电时可用）

P4 没有电池，`s_vbus_present` 固定为 true；睡眠白噪音不会因“拔掉 USB”停止。

## 输入

- BOOT 键 GPIO35：单击切换主题，长按打开/关闭菜单。
- 触摸：左右滑动至少 172 px；快速轻扫至少 86 px、300 ms 内完成且松手前 80 ms 有移动；水平/垂直轴比 2.0；两次切换冷却 150 ms。
- 长按约 2 秒打开菜单；移动超过 28 px 取消长按资格。

阈值按 800 面板从 S3 的 100/50/16 px 等比放大。

## 构建与验证

P4 v1.0 配置要点：

- `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`
- `CONFIG_ESP32P4_REV_MIN_100=y`
- `CONFIG_SPIRAM_SPEED_200M=y`
- `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y`
- `CONFIG_PM_SLEEP_CLK_ICG_ENABLE=n`（应用不使用 light sleep；P4 v1.0 启动时该 retention 初始化可能失败）

```bash
cd esp32p4/clock
./dev.sh build
./dev.sh flash-monitor
```

主机预览和测试：

```bash
./tools/preview/run.sh
ctest --test-dir tools/preview/build --output-on-failure
```

当前已验证：

- ESP-IDF 6.1 按 ESP32-P4 v1.0、360 MHz CPU、200 MHz PSRAM 构建通过。
- 已通过 `/dev/ttyACM0` 烧录，bootloader 识别 v1.0，PSRAM 32 MB @ 200 MHz，CPU 360 MHz。
- 800×800 显示、GT911 触摸、ES8311 扬声器初始化成功。
- ESP-Hosted 以 SDIO slot 1、4-bit、40 MHz 连接 C6，读到 STA MAC。
- 触摸滑动可切换主题；日志显示 8 个主题切换和 UI 音效播放。
- 修复 Dot Matrix 秒数列索引和冒号网格对齐；秒数 `35` 可完整显示。
- 修复 Audio test 打开即重启：LVGL 任务栈提升到 16 KB，自动打开 Audio test 并连续播放多个测试音后未再复位。
- `esp-clock-p4.bin` 约 2.9 MB，8 MB app 分区剩余约 64%。
- 800×800 主机预览可渲染 8 个主题和 21 个菜单状态。
- `alert_scheduler` 与 `menu_and_themes` 主机测试通过。

仍需继续验证：

- C6 Hosted 固件版本报告为 0.0.0，连接日志提示 Host 2.13.0 与协处理器版本不匹配；需要实际扫描/连接/NTP 测试，必要时更新 C6 固件。
- P4 RTC 掉电保持需要安装 RTC 电池后验证。
- 音频播放时偶尔出现 `i2s_channel_disable: channel has not been enabled yet` 日志，但音效仍能播放，需要后续清理。

## 与 S3 工程的主要差异

| 项目 | S3 clock | P4 clock |
| --- | --- | --- |
| 屏幕 | 466×466 AMOLED | 800×800 IPS 圆屏 |
| 布局 | 466 原生 | 466 设计网格编译期换算到 800 |
| 网络 | ESP32-S3 内置 WiFi | ESP32-C6 + ESP-Hosted SDIO |
| RTC | PCF85063 | P4 内部 RTC + RTC 电池 |
| 电源 | AXP2101 + 电池 | USB 供电，无电池管理 |
| IMU | QMI8658 + 自动旋转 | 无 IMU，无 Motion 页 |
| 按键 | GPIO0 + PWR/EXIO4 | BOOT GPIO35 |
| 主题字体 | 144/96/36 px 等 | 248/165/62/185/137/48/41 px |
