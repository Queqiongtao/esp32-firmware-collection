# waveshare_amoled_1_8

ESP-IDF project skeleton for the Waveshare ESP32-S3-Touch-AMOLED-1.8 board.

This project is an independent local implementation focused on board bring-up and
basic infrastructure. It is not a direct copy of the vendor demo, and it does
not depend on external Waveshare BSP components.

## Board

- Board: Waveshare ESP32-S3-Touch-AMOLED-1.8
- Chip: ESP32-S3
- Flash: 16 MB
- PSRAM: enabled, octal mode

## Reference Demo

Official Waveshare demo repository:

- <https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8>

The upstream repository is used only as a reference for hardware information,
pin confirmation, and initialization flow. This project does not directly copy
or depend on that repository's code.

## Current Status

The current codebase is still a board-level skeleton. It currently provides:

- `dev.sh` helper script for build, flash, monitor, clean, and menuconfig
- local `board_amoled_1_8` component
- I2C master bus initialization
- PWM backlight initialization and brightness control
- boot logs for chip and board configuration
- placeholder display initialization entry for later panel/touch bring-up

The following are not implemented yet:

- specific display panel controller initialization
- touch controller initialization
- full graphics or UI application
- migration of the official demo application logic

## Default Board Configuration

Current defaults from `sdkconfig.defaults`:

- I2C port: `0`
- I2C SDA GPIO: `15`
- I2C SCL GPIO: `14`
- I2C frequency: `400000`
- Backlight GPIO: `6`
- Backlight LEDC channel: `0`
- Backlight LEDC timer: `0`
- Backlight PWM frequency: `5000`

## Commands

Run commands from this project directory:

```bash
./dev.sh build
./dev.sh ports
./dev.sh flash-monitor
./dev.sh -p /dev/ttyUSB0 flash-monitor
./dev.sh menuconfig
```

## Key Files

- `main/main.c`: app entry, startup logs, board init call
- `components/board_amoled_1_8/`: local board support skeleton
- `sdkconfig.defaults`: default target and board-related configuration
