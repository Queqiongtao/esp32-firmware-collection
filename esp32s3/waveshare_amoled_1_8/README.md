# waveshare_amoled_1_8

ESP-IDF project skeleton for the Waveshare ESP32-S3-Touch-AMOLED-1.8 board.

This project is focused on board bring-up and basic infrastructure. It keeps a
local `board_amoled_1_8` wrapper component, and it now allows depending on the
Waveshare BSP where that is the most reliable path for board initialization.

## Board

- Board: Waveshare ESP32-S3-Touch-AMOLED-1.8 V1
- Chip: ESP32-S3
- Flash: 16 MB
- PSRAM: enabled, octal mode
- Display controller: SH8601
- Touch controller: FT3168

## Reference Demo

Official Waveshare demo repository:

- <https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8>

The upstream repository is used as a hardware and initialization reference. The
current project may also depend on Waveshare managed BSP components through the
ESP-IDF component manager for SH8601 display bring-up on the V1 board.

## Current Status

The current codebase is a board bring-up project with a local board support
wrapper. It currently provides:

- `dev.sh` helper script for build, flash, monitor, clean, and menuconfig
- local `board_amoled_1_8` component wrapping Waveshare BSP display/touch bring-up
- I2C master bus initialization
- SH8601 QSPI display initialization and brightness control
- FT3168 touch initialization via the FT5x06-compatible touch path
- boot logs and display self-test
- board-level self-test hooks for PMU, SD card, RTC, IMU, and audio

The following are not implemented yet:

- full graphics or UI application
- a production-ready application flow beyond board self-test
- deeper validation and feature polish for PMU, RTC, IMU, SD card, and audio

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

- `main/main.cpp`: app entry, startup logs, display/touch-first self-test flow
- `components/board_amoled_1_8/`: local board support skeleton
- `sdkconfig.defaults`: default target and board-related configuration
