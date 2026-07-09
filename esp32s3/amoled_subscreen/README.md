# amoled_subscreen

ESP-IDF small companion display project for the Waveshare
ESP32-S3-Touch-AMOLED-1.8 board.

This project starts as a clean application skeleton for a future small secondary
screen. It keeps the local `board_amoled_1_8` board wrapper from the bring-up
project, while the application layer only initializes the AMOLED panel and draws
a simple placeholder frame.

## Board

- Board: Waveshare ESP32-S3-Touch-AMOLED-1.8 V1
- Chip: ESP32-S3
- Flash: 16 MB
- PSRAM: enabled, octal mode
- Display controller: SH8601
- Touch controller: FT3168

## Reference

Board bring-up reference in this repository:

- `esp32s3/waveshare_amoled_1_8`

USB secondary display planning reference:

- [`doc/usb-extend-screen-reference.md`](doc/usb-extend-screen-reference.md)

## Current Status

The current codebase is a minimal secondary-screen skeleton. It provides:

- `dev.sh` helper script for build, flash, monitor, clean, and menuconfig
- local `board_amoled_1_8` component wrapping board display/touch bring-up
- I2C master bus initialization
- SH8601 QSPI display initialization and brightness control
- boot logs and a simple display placeholder frame

The following are not implemented yet:

- production secondary-screen UI
- host/device data protocol
- touch-driven interaction
- RTC, IMU, SD card, and audio application flows

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

- `main/main.cpp`: app entry, startup logs, display init, placeholder frame
- `components/board_amoled_1_8/`: local board support component
- `sdkconfig.defaults`: default target and board-related configuration
