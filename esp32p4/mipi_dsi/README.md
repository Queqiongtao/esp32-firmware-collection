| Supported Targets | ESP32-P4 |
| ----------------- | -------- |

# MIPI DSI LCD Panel Example

[esp_lcd](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/peripherals/lcd/dsi_lcd.html) supports MIPI DSI interfaced LCD panel, with frame buffer(s) managed by the driver itself.

This project is adapted from the official example for an ESP32-P4 Function-EV-Board style MIPI setup and currently targets a 1024x600 JD9165 panel with 2-lane DSI.

本工程基于官方示例修改，当前按 ESP32-P4 Function-EV-Board 风格硬件适配，默认目标屏为 1024x600、JD9165、2-lane MIPI DSI。

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

Before testing the display, review your panel timing and resolution settings in [main/mipi_dsi_lcd_example_main.c](./main/mipi_dsi_lcd_example_main.c).

### Configure

Run `idf.py menuconfig` and go to `Example Configuration`:

* Choose the LCD model in `Select MIPI LCD model` according to your board. The current recommended/default target in this repository is `JD9165`.
* Choose whether to `Use DMA2D to copy draw buffer to frame buffer` asynchronously. If you choose `No`, the draw buffer will be copied to the frame buffer synchronously by CPU.
* Choose if you want to `Monitor Refresh Rate by GPIO`. If you choose `Yes`, then you can attach an oscilloscope or logic analyzer to the GPIO pin to monitor the Refresh Rate of the display.
  Please note, the actual Refresh Rate should be **double** the square wave frequency.
* Enable `Bypass PWM and force backlight always on` if you need to distinguish backlight PWM flicker from display timing or panel instability.

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

* This repository is currently tuned for the `JD9165` path with `1024x600`, `RGB565`, `2-lane DSI`, `550 Mbps lane rate`, and `58 MHz DPI clock`.
* If the screen appears to "flicker", first use `Example Configuration -> Bypass PWM and force backlight always on` to determine whether the issue is PWM dimming or display pipeline/panel instability.

* 当前仓库已按 `JD9165`、`1024x600`、`RGB565`、`2-lane DSI`、`550 Mbps lane rate`、`58 MHz DPI clock` 进行适配。
* 如果你看到“背光闪”或者“屏幕闪”，优先启用 `Example Configuration -> Bypass PWM and force backlight always on`，先区分是 PWM 背光问题还是显示链路/时序不稳定。

### Example Output

```bash
...
I (1629) example: MIPI DSI PHY Powered on
I (1629) example: Install MIPI DSI LCD control panel
I (1639) ili9881c: ID1: 0x98, ID2: 0x81, ID3: 0x5c
I (1779) example: Install MIPI DSI LCD data panel
I (1799) example: Initialize LVGL library
I (1799) example: Allocate separate LVGL draw buffers from PSRAM
I (1809) example: Use esp_timer as LVGL tick timer
I (1809) example: Create LVGL task
I (1809) example: Starting LVGL task
I (1919) example: Display LVGL Meter Widget
...
```

## Troubleshooting

For any technical queries, please open an [issue](https://github.com/espressif/esp-idf/issues) on GitHub. We will get back to you soon.
