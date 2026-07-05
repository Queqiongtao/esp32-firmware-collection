| Supported Targets | ESP32-P4 |
| ----------------- | -------- |

# JC1060P470 综合测试工程

这是一个基于 ESP-IDF 的 ESP32-P4 综合测试工程，由 `mipi_dsi` 工程衍生而来。工程以 MIPI DSI 显示（JD9165 1024x600）+ GT911 触摸为基础，逐步集成 SD 卡、USB、摄像头、传感器、WiFi/蓝牙（需 ESP32-C6 协处理器）、音频 I2S 等外设的综合测试能力。

> **芯片版本**：本工程基于 ESP32-P4 **V1.0**（`CONFIG_ESP32P4_REV_MIN_FULL=100`，`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`）。IDF 使用 `register/hw_ver1/` 寄存器定义，不兼容 V3 及以上修订。

显示链路参数与 `mipi_dsi` 工程完全一致，复用本地 JD9165 面板驱动（`components/esp_lcd_jd9165/`）。

## 当前状态

- **已上板验证**：MIPI DSI 显示（JD9165 1024x600 RGB565 2-lane）、GT911 触摸、LVGL 9.4 综合 demo UI（DIRECT 渲染 + 双 framebuffer）、背光 PWM、SD 卡（SDMMC SLOT_0 40MHz 4-bit，已完成挂载与读写回环）、USB Device CDC-ACM、音频 I2S（ES8311，支持内置音乐播放 / 录音 / 录音回放）、板载 RTC（RX8025T-UC，启动校验、非法时间自动同步构建时间、5 秒周期读取）、电池电压采样（GPIO52 ADC）、无线测试模块（P4 <-> C6 SDIO 板级连接 + hosted WiFi STA 路径 + UI 操作卡片）。显示方案详见 [docs/display_solution.md](./docs/display_solution.md)，硬件引脚详见 [docs/hardware_pinout.md](./docs/hardware_pinout.md)。
- **规划中**：摄像头、传感器、BLE hosted 业务联调。

## 当前综合 Demo 页面

当前页面已不是原始 `mipi_dsi` 示例动画页，而是面向板级联调的综合验证界面，默认包含以下 7 张卡片：

- `1. 显示与触摸`
- `2. RTC`
- `3. USB Device`
- `4. 音频`
- `5. BAT`
- `6. WiFi`
- `7. BLE`

其中 WiFi 卡片已接入真实 hosted WiFi 扫描/连接路径；BLE 卡片当前仅展示 hosted 状态，尚未开放业务操作。

## 已知限制与告警

- Hosted WiFi 主链路已可用，但 `GetCoprocessorFwVersion` 等 RPC 元信息请求仍可能超时；这类日志目前表示协处理器元信息通道兼容性不足，不等同于 WiFi 数据链路失败。
- 启动日志中的 `Version mismatch: Host [2.12.0] > Co-proc [0.0.0]` 目前属于版本告警；在现阶段，WiFi STA 路径可继续启动、扫描和连接验证。
- BLE hosted 业务路径尚未接入，当前仅保留状态展示与占位说明，不应视为 BLE 功能已完成。
- TinyUSB 若打印 `No Device descriptor provided, using default.` 等 warning，表示当前工程使用默认 descriptor；只要 CDC-ACM 能正常枚举和回显，这不是功能故障。

## How to use the example

### Hardware Required

* An ESP32-P4 board with MIPI DSI support, preferably an ESP32-P4 Function-EV-Board compatible setup
* A 1024x600 MIPI DSI LCD panel/sub-board using JD9165
* A USB cable for power supply, flashing and serial log output

硬件建议：

* ESP32-P4 开发板，建议使用与 ESP32-P4 Function-EV-Board 接线兼容的硬件
* 1024x600 的 JD9165 MIPI 屏或配套子板
* USB 线，用于供电、烧录和串口日志输出
* 当前板上有两个独立的 USB Device 口：`Full-Speed USB Device`（专用封装脚 `49/50`）和 `High-Speed USB Device`（信号 GPIO `GPIO24/25`，对应封装脚 `52/53`）

### Hardware Connection

Use the following board-level wiring for this project:

| Signal | Current Project Wiring |
| ------ | ---------------------- |
| 5V     | Board 5V -> LCD/sub-board 5V |
| GND    | Board GND -> LCD/sub-board GND |
| Backlight / PWM | `GPIO23` |
| LCD Reset | `GPIO27` |
| Touch Reset (optional) | `GPIO22` |
| Touch INT (optional) | `GPIO21` |
| MIPI DSI | Connect the LCD FPC to the `MIPI_DSI` connector |
| SD Card (SDMMC) | D0-D3=`GPIO39-42`, CLK=`GPIO43`, CMD=`GPIO44` (SLOT_0, enable via menuconfig) |
| RTC | RX8025T-UC shares touch I2C: SDA=`GPIO7`, SCL=`GPIO8`, address `0x32`, reads every 5s when enabled |

For the on-board `ESP32-P4 + ESP32-C6` design, hosted WiFi uses `SDMMC Slot 1` over `GPIO14~19`, while the SD card stays on `SDMMC Slot 0` over `GPIO39~44`. On ESP-IDF `v6.0.x`, the SD card path must reuse the SDMMC host controller already initialized by ESP-Hosted, instead of reinitializing the shared SDMMC host controller again.

当前工程建议按下表连接：

| 信号 | 当前工程接线 |
| ---- | ------------ |
| 5V | 开发板 5V 接屏或子板 5V |
| GND | 开发板 GND 接屏或子板 GND |
| 背光 / PWM | `GPIO23` |
| LCD 复位 | `GPIO27` |
| 触摸复位（可选） | `GPIO22` |
| 触摸中断（可选） | `GPIO21` |
| MIPI DSI | 屏排线接到 `MIPI_DSI` 接口 |
| SD 卡 (SDMMC) | D0-D3=`GPIO39-42`, CLK=`GPIO43`, CMD=`GPIO44`（SLOT_0，menuconfig 启用） |
| RTC | RX8025T-UC 复用触摸 I2C：SDA=`GPIO7`, SCL=`GPIO8`, 地址 `0x32`，启用后每 5 秒读取一次 |
| USB Device | `High-Speed USB Device` 口用于当前 TinyUSB CDC-ACM echo 测试（信号 GPIO `GPIO24/25`，对应封装脚 `52/53`）；`Full-Speed USB Device` 口当前未被本工程使用 |
| WiFi/BLE 协处理器 | `ESP32-P4 <-> ESP32-C6` 通过 SDIO 相连：D0=`GPIO14`, D1=`GPIO15`, D2=`GPIO16`, D3=`GPIO17`, CLK=`GPIO18`, CMD=`GPIO19` |

当前板上 `ESP32-C6` hosted WiFi 走 `SDMMC Slot 1`，SD 卡走 `SDMMC Slot 0`。在 ESP-IDF `v6.0.x` 下，SD 卡测试路径需要复用 ESP-Hosted 已完成的 SDMMC host controller 初始化，不能再次重复初始化共享的 SDMMC host controller。

Before testing the display, review the active display parameters in [main/display_config.h](./main/display_config.h).

### Configure

Run `idf.py menuconfig` and go to `Example Configuration`:

* Choose whether to `Use DMA2D to copy draw buffer to frame buffer` asynchronously. If you choose `No`, the draw buffer will be copied to the frame buffer synchronously by CPU.
* Enable `Bypass PWM and force backlight always on` if you need to distinguish backlight PWM flicker from display timing or panel instability.
* Adjust `LVGL draw buffer lines` and `LVGL draw buffer memory` to compare PSRAM/internal RAM tradeoffs without changing source code.
* Enable `WiFi/BLE test module` if you want the integrated demo UI to show the wireless card and initialize the hosted WiFi test path over the on-board `ESP32-C6`.
* For the two-chip WiFi path, set `Component config -> Wi-Fi Remote -> choose slave target` to `esp32c6`.

### Build and Flash

This project provides [dev.sh](./dev.sh), a small wrapper around `eim run "idf.py ..."`. It uses the ESP-IDF version currently selected by `eim`, so you do not need to source ESP-IDF's `export.sh` manually in each shell.

Common commands:

```bash
./dev.sh build
./dev.sh flash -p PORT
./dev.sh monitor -p PORT
./dev.sh flash-monitor -p PORT
./dev.sh menuconfig
```

`PORT` is the serial device connected to the board, for example `/dev/ttyUSB0`. The `flash-monitor` command is equivalent to running `idf.py -p PORT flash monitor` inside the selected `eim` ESP-IDF environment.

The script writes the `eim` log to `/tmp/eim-JC1060P470.log` by default. You can override it when needed:

```bash
EIM_LOG_FILE=/tmp/my-eim.log ./dev.sh build
```

For uncommon `idf.py` commands, use the pass-through form:

```bash
./dev.sh idf -p PORT build flash monitor
```

A LVGL widget should show up on the LCD as expected after flashing.

The first time you run `idf.py` for the example will cost extra time as the build system needs to address the component dependencies and downloads the missing components from the ESP Component Registry into `managed_components` folder.

(To exit the serial monitor, type ``Ctrl-]``.)

See the [Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html) for full steps to configure and use ESP-IDF to build projects.

## Hardware Notes / 硬件注意事项

* Do not hot-plug the LCD FPC while the board is powered.
* Confirm the FPC orientation and that the connector is fully locked before power-on.
* Make sure the display sub-board and the ESP32-P4 board share a common ground.
* The current project expects MIPI DPHY power from internal `LDO channel 3` at `2.5V`.
* For flashing and logs, connect the PC to the board `USB-UART` port, then power on the board in the normal sequence.

* 屏排线不要在上电状态下热插拔。
* 上电前确认 FPC 方向正确，连接器已经完全压紧锁定。
* 屏子板与 ESP32-P4 开发板必须共地。
* 当前工程默认使用内部 `LDO channel 3` 输出 `2.5V` 作为 MIPI DPHY 供电。
* 烧录和看日志时，先连接开发板 `USB-UART`，再按正常顺序上电。
* 当前板上两个 USB Device 口都可能在主机侧表现为 USB 设备；本工程的 CDC echo 测试请连接 `High-Speed USB Device` 口（信号 GPIO `GPIO24/25`，对应封装脚 `52/53`），不要把它和烧录/日志口混用。
* 当前板上 `GPIO14~19` 已固定连接到 `ESP32-C6`，当前 hosted WiFi 已使用这组 SDIO 连接，不要复用这些 IO。

## Project Notes / 工程说明

* This project is derived from the `mipi_dsi` project and tuned for `JD9165`, `1024x600`, `RGB565`, `2-lane DSI`, `750 Mbps lane rate`, `54 MHz DPI clock`, and `2` DPI frame buffers.
* If the screen appears to "flicker", first use `Example Configuration -> Bypass PWM and force backlight always on` to determine whether the issue is PWM dimming or display pipeline/panel instability.
* The display stack is split into board control, panel bring-up, and LVGL port layers:
  * [main/display_board.c](./main/display_board.c): PHY power, backlight
  * [main/display_panel.c](./main/display_panel.c): DSI bus, DBI IO, JD9165 panel creation
  * [main/display_lvgl.c](./main/display_lvgl.c): LVGL task, draw buffers, flush callbacks

* 本工程由 `mipi_dsi` 工程衍生，已按 `JD9165`、`1024x600`、`RGB565`、`2-lane DSI`、`750 Mbps lane rate`、`54 MHz DPI clock`、`2` 个 DPI framebuffer 进行适配。
* 如果你看到“背光闪”或者“屏幕闪”，优先启用 `Example Configuration -> Bypass PWM and force backlight always on`，先区分是 PWM 背光问题还是显示链路/时序不稳定。
* 显示链路代码已经拆成三层：
  * [main/display_board.c](./main/display_board.c)：PHY 供电、背光
  * [main/display_panel.c](./main/display_panel.c)：DSI 总线、DBI IO、JD9165 面板创建
  * [main/display_lvgl.c](./main/display_lvgl.c)：LVGL 任务、draw buffer、flush 回调
* 无线模块已按独立测试模块接入：
  * [main/test_wireless.c](./main/test_wireless.c)：无线状态机、板级 SDIO 定义、WiFi STA 初始化/扫描/连接接口、BLE 未接入时的统一返回路径
  * [main/test_config.h](./main/test_config.h)：`GPIO14~19` 的 `P4 <-> C6 SDIO` 宏定义
  * Host 侧依赖 `espressif/esp_hosted` 与 `espressif/esp_wifi_remote`
  * 当前已接入真实 WiFi hosted 业务路径；BLE 仍未接入，UI 仅保留状态展示
* SD 卡模块已按 `ESP-Hosted + SDMMC` 共存路径接入：
  * 当前板上 `ESP32-C6` hosted WiFi 走 `SDMMC Slot 1`
  * SD 卡走 `SDMMC Slot 0`
  * 在 ESP-IDF `v6.0.x` 下，SD 卡测试路径需复用 ESP-Hosted 已初始化的共享 SDMMC host controller，不能重复初始化

### Example Output

```bash
...
I (22xx) display_lvgl: Initialize LVGL
I (22xx) app_main: Display LVGL integrated demo UI
I (23xx) test_sdcard: Hosted+SDMMC workaround: enabled
I (24xx) test_sdcard: Filesystem mounted
I (24xx) test_sdcard: Test file removed, SD card read/write test passed
I (24xx) test_rtc: RX8025T-UC periodic read task started, interval=5000 ms
I (25xx) test_audio: Audio initialized (sample_rate=16000, volume=60%, mclk_multiple=384, record_buf=320000 bytes)
I (25xx) test_battery: Battery ADC initialized on GPIO52 (ADC unit 2 channel 3), divider=68000/100000, calibration=enabled
I (28xx) test_usb: USB CDC-ACM initialized on High-Speed USB Device port, waiting for host enumeration
I (47xx) test_wireless: Hosted transport ready, continue WiFi path
I (69xx) test_wireless: WiFi hosted path verified on P4<->C6 SDIO
I (70xx) RPC_WRAP: ESP Event: wifi station started
W (67xx) test_wireless: Hosted CP metadata unavailable, continue with WiFi path
...
```

## Troubleshooting

For any technical queries, please open an [issue](https://github.com/espressif/esp-idf/issues) on GitHub. We will get back to you soon.
