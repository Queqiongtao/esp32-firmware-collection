| Supported Targets | ESP32-P4 |
| ----------------- | -------- |

# JC1060P470 综合测试工程

这是一个基于 ESP-IDF 的 ESP32-P4 综合测试工程，由 `mipi_dsi` 工程衍生而来。工程以 MIPI DSI 显示（JD9165 1024x600）+ GT911 触摸为基础，逐步集成 SD 卡、USB、摄像头、传感器、WiFi/蓝牙（需 ESP32-C6 协处理器）、音频 I2S 等外设的综合测试能力。

> **芯片版本**：本工程基于 ESP32-P4 **V1.0**（`CONFIG_ESP32P4_REV_MIN_FULL=100`，`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`）。IDF 使用 `register/hw_ver1/` 寄存器定义，不兼容 V3 及以上修订。

显示链路参数与 `mipi_dsi` 工程完全一致，复用本地 JD9165 面板驱动（`components/esp_lcd_jd9165/`）。

## 当前状态

- **当前实现**：显示/触摸、SD、RTC、音频、电池、HIGH USB CDC/UVC、USB 角色管理和 Hosted WiFi/BLE 均按独立 capability 模块接入；MIPI CSI 摄像头和通用传感器尚未实现。
- **当前默认**：HIGH USB 编译 UVC Host，role worker 启动保持 None；UVC 默认稳定偏好为 `1280x720`，UI 可手动选择设备枚举出的其他 MJPEG 分辨率。
- **验证边界**：显示、基础外设、Hosted WiFi、历史 CDC 和部分 UVC 路径有上板记录；当前 `640x360` UVC 日志记录为零错误连续发布。动态切换、自动恢复、BLE 和反复启停仍按待验证处理。
- **规划中**：MIPI CSI 摄像头和通用传感器模块。

设计说明见 [显示方案](./docs/display_solution.md)、[USB 当前方案](./docs/usb_solution.md) 和
[外设测试说明](./docs/peripheral_tests.md)；所有上板证据、历史实验与待验收项统一见
[验证状态与实验记录](./docs/validation_status.md)。

## 当前综合 Demo 页面

当前页面已不是原始 `mipi_dsi` 示例动画页，而是面向板级联调的综合验证界面，默认包含以下 8 张卡片：

- `1. 显示与触摸`
- `2. RTC`
- `3. HIGH USB`
- `4. 音频`
- `5. BAT`
- `6. WiFi`
- `7. BLE`
- `8. FreeType`

其中 WiFi 卡片接入 hosted WiFi 扫描/连接路径；BLE 卡片接入广播和 GATT 测试路径。HIGH USB 卡片只启停当前编译进固件的 UVC Host 或 CDC Device capability，不在运行时安装未编译的另一套能力。

## 已知限制与告警

- Hosted WiFi 主链路已可用，但 `GetCoprocessorFwVersion` 等 RPC 元信息请求仍可能超时；这类日志目前表示协处理器元信息通道兼容性不足，不等同于 WiFi 数据链路失败。
- 启动日志中的 hosted Host/Co-proc 版本不匹配属于版本告警；具体版本取决于当前 P4 组件和 ESP32-C6 固件，不应把某一组日志版本当成工程固定接口。在现阶段，WiFi STA 路径可继续启动、扫描和连接验证。
- BLE hosted 业务路径已实现，但当前缺少足够的广播、连接和 GATT 回归证据，不应表述为稳定可用。
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
* 当前板上有两个 USB Type-C 口，但能力不同：`FULL USB`（丝印，信号 GPIO `GPIO24/25`）是 USB Serial/JTAG（USJ）固定功能 CDC-ACM 串口，用于烧录和日志；`HIGH USB`（丝印，专用封装脚 `49/50`）是内部 UTMI High-Speed PHY + DWC OTG controller[0]，480Mbps，可作 HS Device（TinyUSB CDC-ACM echo）或 HS Host（usb_host + UVC），同一时刻只能选一种角色。USB 子系统方案详见 [docs/usb_solution.md](./docs/usb_solution.md)。

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
| Shared I2C | GT911, RX8025T-UC and ES8311 share board-owned I2C1: SDA=`GPIO7`, SCL=`GPIO8`; GT911/RTC device speed `400 kHz`, ES8311 component speed `100 kHz` |

For the on-board `ESP32-P4 + ESP32-C6` design, hosted WiFi uses `SDMMC Slot 1` over `GPIO14~19`, while the SD card stays on `SDMMC Slot 0` over `GPIO39~44`. On the pinned ESP-IDF `v6.0.1`, the SD card path must reuse the SDMMC host controller already initialized by ESP-Hosted, instead of reinitializing the shared SDMMC host controller again.

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
| 共享 I2C | GT911、RX8025T-UC 和 ES8311 复用 board 层持有的 I2C1：SDA=`GPIO7`、SCL=`GPIO8`；GT911/RTC device 速率 `400 kHz`，ES8311 component 速率 `100 kHz` |
| HIGH USB | 专用封装脚 `49/50`、内部 UTMI HS PHY、DWC OTG controller[0]；默认编译 UVC Host，也可在 Kconfig 中改选 TinyUSB CDC Device。`FULL USB`（`GPIO24/25`，USJ）仍只用于烧录/日志。详见 [docs/usb_solution.md](./docs/usb_solution.md) |
| WiFi/BLE 协处理器 | `ESP32-P4 <-> ESP32-C6` 通过 SDIO 相连：D0=`GPIO14`, D1=`GPIO15`, D2=`GPIO16`, D3=`GPIO17`, CLK=`GPIO18`, CMD=`GPIO19` |

当前板上 `ESP32-C6` hosted WiFi 走 `SDMMC Slot 1`，SD 卡走 `SDMMC Slot 0`。在固定的 ESP-IDF `v6.0.1` 下，SD 卡测试路径需要复用 ESP-Hosted 已完成的 SDMMC host controller 初始化，不能再次重复初始化共享的 SDMMC host controller。

Before testing the display, review the active display parameters in [main/display_config.h](./main/display_config.h).

### Configure

Run `idf.py menuconfig` and go to `Example Configuration`:

* Choose whether to `Use ESP LCD DMA2D draw_bitmap hook`. This enables the ESP LCD DPI panel DMA2D copy hook and is separate from LVGL's STM32-only `LV_USE_DRAW_DMA2D`.
* Choose `LVGL port implementation`. The default is `Espressif esp_lvgl_adapter`; `Project legacy LVGL port` keeps the previous custom DIRECT dual-framebuffer path as a fallback.
* Choose `esp_lvgl_adapter tear avoidance mode`. The default is `DOUBLE_DIRECT`; `TRIPLE_PARTIAL` and `DOUBLE_FULL` are available for controlled comparison.
* Enable `Bypass PWM and force backlight always on` if you need to distinguish backlight PWM flicker from display timing or panel instability.
* Adjust `LVGL draw buffer lines` and `LVGL draw buffer memory` to compare PSRAM/internal RAM tradeoffs without changing source code.
* Enable `WiFi/BLE test module` to initialize the shared hosted transport over the on-board `ESP32-C6`. The WiFi status/actions and BLE status/actions sub-options are independent, so WiFi-only, BLE-only, and combined WiFi+BLE builds are all supported; the demo UI enables only the capabilities compiled into the firmware.
* Select the single HIGH USB capability. The default is UVC Host with a stable `1280x720` preference; CDC Device is an alternative build-time choice. The role worker normally starts at None and occupies controller[0] after a UI request; automatic UVC recovery after an interrupted warm reset is the documented exception.
* For the two-chip WiFi path, set `Component config -> Wi-Fi Remote -> choose slave target` to `esp32c6`.

### Build and Flash

This project provides [dev.sh](./dev.sh), a small wrapper around `eim run "idf.py ..."`. It uses the ESP-IDF version currently selected by `eim`, so you do not need to source ESP-IDF's `export.sh` manually in each shell.

The project pins ESP-IDF to **6.0.1** (`main/idf_component.yml`). Select that exact version before the first configure/build; do not rely on whichever IDF happens to be active in the shell:

```bash
eim list
eim select 6.0.1
```

If 6.0.1 is not installed yet, install it first and then select it:

```bash
eim install --idf-versions 6.0.1
eim select 6.0.1
```

`./dev.sh` checks that `eim` exists, but it intentionally does not select a version for you. Verify the selected version with `eim list` or `eim run "idf.py --version"` before building.

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

After a successful full flash and board power-up, the LVGL demo should appear on the LCD. A build result alone does not establish display, UVC, USB role, or wireless stability; check [docs/validation_status.md](./docs/validation_status.md) for the exact validation boundary.

The first time you run `idf.py` for the example will cost extra time as the build system needs to address the component dependencies and downloads the missing components from the ESP Component Registry into `managed_components` folder.

Managed dependencies are conditional on the selected Kconfig capability: Audio resolves `esp_codec_dev`, CDC resolves TinyUSB, UVC resolves `usb_host_uvc`, and Wireless resolves the hosted/WiFi components. `main/CMakeLists.txt` still lists some built-in IDF components for dependency discovery; this does not mean every optional source or managed library is linked into the image. Changing the CDC/UVC choice or disabling an external peripheral requires reconfigure/build so the resolved graph and `dependencies.lock` match the selected configuration.

On a clean configure, Kconfig can print first-pass `unknown kconfig symbol` warnings for TinyUSB,
UVC or Hosted options before their conditional managed components enter the dependency graph. The
component manager reruns Kconfig after resolving dependencies; judge the configuration by the final
report and generated `sdkconfig`. The TinyUSB CDC defaults are intentionally retained because
`esp_tinyusb 2.2.0` otherwise leaves the alternative CDC capability disabled.

The custom partition table reserves a `4 MiB` factory app and a `2 MiB` `font` SPIFFS partition (`partitions.csv`). Because the font image is added with `FLASH_IN_PROJECT`, use a complete `flash` after changing partitions, font assets, or the selected dependency set:

```bash
./dev.sh flash -p PORT
```

An app-only flash does not update the partition table or the `font` image and can leave the device booting an incompatible layout or stale font data. The target flash is 16 MiB; keep the generated partition table and font image within that boundary.

(To exit the serial monitor, type ``Ctrl-]``.)

See the [Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html) for full steps to configure and use ESP-IDF to build projects.

## Hardware Notes / 硬件注意事项

* Do not hot-plug the LCD FPC while the board is powered.
* Confirm the FPC orientation and that the connector is fully locked before power-on.
* Make sure the display sub-board and the ESP32-P4 board share a common ground.
* The current project expects MIPI DPHY power from internal `LDO channel 3` at `2.5V`.
* For flashing and logs, connect the PC to the board `FULL USB` port (USB Serial/JTAG), then power on the board in the normal sequence.

* 屏排线不要在上电状态下热插拔。
* 上电前确认 FPC 方向正确，连接器已经完全压紧锁定。
* 屏子板与 ESP32-P4 开发板必须共地。
* 当前工程默认使用内部 `LDO channel 3` 输出 `2.5V` 作为 MIPI DPHY 供电。
* 烧录和看日志时，先连接开发板 `FULL USB`（USB Serial/JTAG），再按正常顺序上电。
* 当前板上两个 USB Type-C 口都可能被主机侧识别为串口设备，但能力不同：`FULL USB`（`GPIO24/25`，USJ 控制器）只用于烧录/日志；`HIGH USB`（封装脚 `49/50`，UTMI HS PHY + DWC OTG controller[0]）只包含 Kconfig 所选的 CDC Device 或 UVC Host capability，运行时只能在该 capability 与 None 之间切换。CDC echo 测试请使用 CDC 构建并连接 `HIGH USB` 口，不要与烧录/日志口混用。
* 当前板上 `GPIO14~19` 已固定连接到 `ESP32-C6`，当前 hosted WiFi 已使用这组 SDIO 连接，不要复用这些 IO。

## Project Notes / 工程说明

* This project is derived from the `mipi_dsi` project and tuned for `JD9165`, `1024x600`, `RGB565`, `2-lane DSI`, `750 Mbps lane rate`, `54 MHz DPI clock`, and `2` DPI frame buffers.
* The active display stack uses `lvgl/lvgl 9.4.0` and `espressif/esp_lvgl_adapter ^0.6.2`. Adapter PPA is enabled, ESP LCD DMA2D hook is enabled, and LVGL built-in `CONFIG_LV_USE_PPA` is disabled.
* If the screen appears to "flicker", first use `Example Configuration -> Bypass PWM and force backlight always on` to determine whether the issue is PWM dimming or display pipeline/panel instability.
* The display stack is split into board control, panel bring-up, and LVGL port layers:
  * [main/display_board.c](./main/display_board.c): PHY power, backlight
  * [main/display_panel.c](./main/display_panel.c): DSI bus, DBI IO, JD9165 panel creation, DMA2D hook
  * [main/display_lvgl.c](./main/display_lvgl.c): esp_lvgl_adapter display/touch registration, legacy LVGL port fallback

* 本工程由 `mipi_dsi` 工程衍生，已按 `JD9165`、`1024x600`、`RGB565`、`2-lane DSI`、`750 Mbps lane rate`、`54 MHz DPI clock`、`2` 个 DPI framebuffer 进行适配。
* 当前显示栈使用 `lvgl/lvgl 9.4.0` 与 `espressif/esp_lvgl_adapter ^0.6.2`。已启用 adapter PPA 和 ESP LCD DMA2D hook，未启用 LVGL 内置 `CONFIG_LV_USE_PPA`。
* 工程固定使用 ESP-IDF `6.0.1`，managed component 解析结果记录在 `dependencies.lock`。`esp_codec_dev`、TinyUSB、UVC Host、ESP-Hosted/WiFi Remote 等 managed dependency 按对应 Kconfig 条件解析；CMake 为 IDF 内置组件保留必要的发现依赖，不能把“裁剪”理解为所有可选 built-in 库都从依赖图消失。关闭 Audio 时不会向应用镜像嵌入 `canon.pcm`。
* 如果你看到“背光闪”或者“屏幕闪”，优先启用 `Example Configuration -> Bypass PWM and force backlight always on`，先区分是 PWM 背光问题还是显示链路/时序不稳定。
* 显示链路代码已经拆成三层：
  * [main/display_board.c](./main/display_board.c)：PHY 供电、背光
  * [main/display_panel.c](./main/display_panel.c)：DSI 总线、DBI IO、JD9165 面板创建、DMA2D hook
  * [main/display_lvgl.c](./main/display_lvgl.c)：esp_lvgl_adapter 显示/触摸注册、legacy LVGL port 回退
* 无线模块已按独立测试模块接入：
  * [main/test_wireless.c](./main/test_wireless.c)：无线状态机、WiFi STA 初始化/扫描/连接接口，以及 hosted NimBLE 广播和 GATT 测试
  * [main/test_config.h](./main/test_config.h)：`GPIO14~19` 的 `P4 <-> C6 SDIO` 宏定义
  * Host 侧依赖 `espressif/esp_hosted` 与 `espressif/esp_wifi_remote`
  * WiFi 已有既有上板记录；BLE 及无线重连/释放路径仍待上板回归
* SD 卡模块已按 `ESP-Hosted + SDMMC` 共存路径接入：
  * 当前板上 `ESP32-C6` hosted WiFi 走 `SDMMC Slot 1`
  * SD 卡走 `SDMMC Slot 0`
  * 在 ESP-IDF `v6.0.1` 下，SD 卡测试路径需复用 ESP-Hosted 已初始化的共享 SDMMC host controller，不能重复初始化

## Troubleshooting

For any technical queries, please open an [issue](https://github.com/espressif/esp-idf/issues) on GitHub. We will get back to you soon.
