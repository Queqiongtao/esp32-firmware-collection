# esp-clock-p4

ESP32-P4 800×800 圆屏时钟固件，目标硬件为 [Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C](https://www.waveshare.net/shop/ESP32-P4-WIFI6-Touch-LCD-3.4C.htm)。

本工程从 `esp32s3/clock` 移植，但按 P4 板卡重新适配：P4 无 IMU、无 AXP2101 电池管理，WiFi 通过板载 ESP32-C6 + ESP-Hosted 提供，RTC 使用 ESP32-P4 内部 RTC。UI 直接使用 800×800 原生坐标，不再通过父容器缩放。

详细中文说明见 [PROJECT_OVERVIEW.md](PROJECT_OVERVIEW.md)。

## 功能

- 800×800 圆屏时钟，8 个可切换主题：Minimal、Segment、Analog、Dot Matrix、M5 Classic、M5 Simple、M5 Big Number、M5 Number Flow
- 主题索引保存在 NVS，重启恢复
- 左右滑动/轻扫切换主题，约 2 秒长按打开设置菜单；主题切换带缩放淡出/淡入过渡
- 设置菜单 4 页：Display、Power、WiFi、Sound；顶部 Done 关闭，1 分钟无操作自动关闭
- Display 页提供两页表盘缩略图选择器，点击选中后 Apply 应用
- Display 页提供亮度和表盘选择；Power 页提供息屏超时和 USB 供电状态，P4 不显示电池卡片
- WiFi 页支持开关、Setup 配网、附近 AP 扫描、Auto NTP 和 Sync now
- ESP32-C6 通过 ESP-Hosted SDIO 提供 WiFi 6；启动时连接 C6 并打印 STA MAC
- NTP 同步系统时间；P4 内部 RTC 在掉电后由 RTC 电池保持（未装电池时使用编译时间兜底）
- ES8311 + ES7210 音频方案，PA 使能 GPIO53，支持 UI 音效、整点报时、3 个闹钟、倒计时和睡眠白噪音
- BOOT 键（GPIO35）单击切换主题、长按打开菜单

## 硬件

- 板卡：Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C
- SoC：ESP32-P4 revision v1.0，360 MHz CPU，32 MB Flash，32 MB PSRAM（200 MHz）
- 显示：800×800 IPS 圆屏，2-lane MIPI-DSI，80 MHz DPI
- 触摸：GT9271/GT911 兼容
- 网络：板载 ESP32-C6，SDIO（slot 1，GPIO14–19，reset GPIO54）
- 音频：ES8311 扬声器 + ES7210 麦克风，PA GPIO53
- RTC：ESP32-P4 内部 RTC + RTC 电池座
- 按键：BOOT（GPIO35）、RESET；无独立 PWR 键、无 IMU、无电池管理

## 构建与烧录

```bash
./dev.sh build
./dev.sh ports
./dev.sh flash-monitor -p /dev/ttyACM0
```

`dev.sh` 通过 `eim` 调用 ESP-IDF，默认日志 `/tmp/eim-clock-p4.log`，可用 `EIM_LOG_FILE` 覆盖。

构建产物：

- 固件：`build/esp-clock-p4.bin`
- 分区表：自定义 8 MB factory app 分区，partition table offset `0x10000`

## 主机预览

```bash
./tools/preview/run.sh
```

预览使用同一批 `clock_theme_*.cpp`、设置菜单和 LVGL 桌面构建，输出 800×800 圆屏 PNG 到 `tools/preview/out/`。它是近似预览，不验证真实亮度、Gamma、触摸、WiFi 和音频；详见 [tools/preview/README.md](tools/preview/README.md)。

## 运行说明

- 工程按 ESP32-P4 v1.0 编译：360 MHz CPU、200 MHz HEX PSRAM，使用 pre-v3 寄存器集；v3.0 的 400 MHz/250 MHz 配置不适用于本板。
- 当前应用不使用 light sleep，`CONFIG_PM_SLEEP_CLK_ICG_ENABLE` 显式关闭，避免 P4 v1.0 启动阶段 sleep retention 初始化失败。
- UI 主题和菜单按 466×466 设计网格编写，通过 `clock_theme_px()` 在编译期换算到 800×800；文字和矢量图形原生渲染，不使用父容器放大。
- 主题字体按 800 布局重新生成：Minimal 248 px、Segment 165/62 px、M5 185/137/48/41 px、菜单大数字 62 px。生成方法见 [tools/fonts/README.md](tools/fonts/README.md)。
- P4 不启用自动旋转和 IMU 采样；`clock_settings` 中不再保存 `auto_rotate`。
- 设置菜单只有 Display、Power、WiFi、Sound 四页；Motion 页和电池卡片已移除。
- WiFi 使用 `esp_wifi_remote` + `esp_hosted`。启动时调用 `esp_hosted_connect_to_slave()` 等待 C6 传输就绪，然后初始化 WiFi STA 并打印 MAC。当前样机日志中协处理器固件版本报告为 `0.0.0`，Host 为 `2.13.0`；基础传输已就绪，但正式使用前应完成扫描/连接/NTP 验证，必要时升级 C6 Hosted 固件。
- RTC 使用 P4 内部 RTC 域；NTP 同步后系统时间自动由 RTC 保持。未安装 RTC 电池时，冷启动使用固件编译时间兜底，联网后由 NTP 校正。
- 睡眠白噪音在 P4 上始终视为 USB 供电可用；音频代码与 S3 相同，但引脚由 BSP 配置。
- 设置保存在 NVS `clock_settings` 命名空间；WiFi 凭据保存在 `wifi_creds` 命名空间。

## 关键文件

- `main/main.cpp`：应用入口、800×800 画布、主题切换/过渡、菜单、WiFi/NTP 状态、音频事件、ESP-Hosted 初始化
- `main/clock_theme.h`：主题接口和 466→800 编译期换算
- `main/clock_theme_*.cpp`：8 个主题
- `main/clock_settings_menu.cpp` / `.h`：4 页设置菜单、WiFi 配网/键盘、Sound/Alarm/Timer/Sleep 弹层
- `main/clock_touch_gesture.cpp` / `.h`：触摸滑动/长按手势层
- `main/wifi_manager.cpp` / `.h`：WiFi STA、NVS 凭据、AP 扫描、断线重连
- `main/ntp_time_sync.cpp` / `.h`：SNTP 启停、手动同步、重试退避
- `main/rtc_time_sync.cpp` / `.h`：P4 内部 RTC 兜底
- `main/audio_manager.cpp` / `.h`：ES8311/PA 音频
- `main/clock_settings.cpp` / `.h`：NVS 设置
- `main/lv_font_*.c`、`main/m5_assets/`：主题字体与图片资源
- `tools/preview/`：主机端 800×800 预览
- `tools/fonts/`：字体和点阵字形生成脚本
