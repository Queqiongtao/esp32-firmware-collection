| Supported Targets | ESP32-P4 |
| ----------------- | -------- |

# MIPI DSI LCD Panel Example

[esp_lcd](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/peripherals/lcd/dsi_lcd.html) supports MIPI DSI interfaced LCD panel, with frame buffer(s) managed by the driver itself.

This project is no longer a generic official demo clone. It is a JD9165-oriented ESP32-P4 display project derived from the official example and tuned for a 1024x600, 2-lane MIPI DSI panel.

本工程已不再保持“官方通用示例”形态，而是基于官方示例收敛成一个面向 JD9165 的 ESP32-P4 显示工程，当前默认目标屏为 1024x600、2-lane MIPI DSI。

## How to use the example

### Hardware Required

* An ESP32-P4 board with MIPI DSI support, preferably an ESP32-P4 Function-EV-Board compatible setup
* A 1024x600 MIPI DSI LCD panel/sub-board using JD9165
* A USB cable for power supply, flashing and serial log output

硬件建议：

* ESP32-P4 开发板，建议使用与 ESP32-P4 Function-EV-Board 接线兼容的硬件
* 1024x600 的 JD9165 MIPI 屏或配套子板
* USB 线，用于供电、烧录和串口日志输出

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

This README documents the active wiring used by the current project. Older Demo README files may mention `GPIO26` for PWM, but this repository's BSP-derived wiring and code use `GPIO23` for backlight control.

本文档以当前工程实际使用的接线为准。旧版 Demo README 中可能出现 `GPIO26` 的 PWM 说明，但本仓库当前代码和板级定义实际使用的是 `GPIO23` 背光控制。

Before testing the display, review the active display parameters in [main/display_config.h](./main/display_config.h).

### Configure

Run `idf.py menuconfig` and go to `Example Configuration`:

* Choose whether to `Use DMA2D to copy draw buffer to frame buffer` asynchronously. If you choose `No`, the draw buffer will be copied to the frame buffer synchronously by CPU.
* Choose if you want to `Monitor Refresh Rate by GPIO`. If you choose `Yes`, then you can attach an oscilloscope or logic analyzer to the GPIO pin to monitor the Refresh Rate of the display.
  Please note, the actual Refresh Rate should be **double** the square wave frequency.
* Enable `Bypass PWM and force backlight always on` if you need to distinguish backlight PWM flicker from display timing or panel instability.
* Adjust `LVGL draw buffer lines` and `LVGL draw buffer memory` to compare PSRAM/internal RAM tradeoffs without changing source code.

### Build and Flash

Run `idf.py -p PORT build flash monitor` to build, flash and monitor the project. A LVGL widget should show up on the LCD as expected.

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

## Project Notes / 工程说明

* This repository is currently tuned for `JD9165`, `1024x600`, `RGB565`, `2-lane DSI`, `750 Mbps lane rate`, `54 MHz DPI clock`, and `2` DPI frame buffers.
* If the screen appears to "flicker", first use `Example Configuration -> Bypass PWM and force backlight always on` to determine whether the issue is PWM dimming or display pipeline/panel instability.
* The display stack is split into board control, panel bring-up, and LVGL port layers:
  * [main/display_board.c](./main/display_board.c): PHY power, backlight, refresh monitor GPIO
  * [main/display_panel.c](./main/display_panel.c): DSI bus, DBI IO, JD9165 panel creation
  * [main/display_lvgl.c](./main/display_lvgl.c): LVGL task, draw buffers, flush callbacks

* 当前仓库已按 `JD9165`、`1024x600`、`RGB565`、`2-lane DSI`、`750 Mbps lane rate`、`54 MHz DPI clock`、`2` 个 DPI framebuffer 进行适配。
* 如果你看到“背光闪”或者“屏幕闪”，优先启用 `Example Configuration -> Bypass PWM and force backlight always on`，先区分是 PWM 背光问题还是显示链路/时序不稳定。
* 显示链路代码已经拆成三层：
  * [main/display_board.c](./main/display_board.c)：PHY 供电、背光、刷新监视 GPIO
  * [main/display_panel.c](./main/display_panel.c)：DSI 总线、DBI IO、JD9165 面板创建
  * [main/display_lvgl.c](./main/display_lvgl.c)：LVGL 任务、draw buffer、flush 回调

### Example Output

```bash
...
I (1629) display_board: MIPI DSI PHY powered on
I (1639) display_panel: Panel=JD9165 res=1024x600 lane_rate=750Mbps dpi_clk=54MHz num_fbs=2
I (1779) display_lvgl: Initialize LVGL
I (1799) display_lvgl: Draw buffers: 80 lines, ...
I (1809) display_lvgl: Starting LVGL task
I (1919) app_main: Display LVGL demo UI
...
```

## Troubleshooting

For any technical queries, please open an [issue](https://github.com/espressif/esp-idf/issues) on GitHub. We will get back to you soon.
