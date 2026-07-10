# amoled_subscreen

ESP-IDF small companion display project for the Waveshare
ESP32-S3-Touch-AMOLED-1.8 board.

This project implements Windows USB secondary-display support for the Waveshare
ESP32-S3-Touch-AMOLED-1.8. It keeps the local `board_amoled_1_8` board wrapper,
receives JPEG frames through a TinyUSB Vendor interface, and rotates the Windows
landscape image onto the physical portrait AMOLED panel.

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
- Windows IDD-compatible USB Vendor display (`VID:PID 303A:2987`)
- JPEG frame queue in PSRAM with `368x448` and `448x368` frame support
- clockwise rotation from landscape frames to the physical `368x448` panel

The following are not implemented yet:

- touch-driven Windows HID input
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

## Windows Extended Display

Install Espressif's USB Extend Screen Windows IDD driver, then connect the
firmware through the board's USB OTG data port. The driver matches the display-
only identity `USB\\VID_303A&PID_2987`; the development driver exposes
`368x448` and `448x368`. Portrait output is direct, while landscape output is
rotated clockwise to the physical panel.

This first version intentionally exposes only the Vendor display interface.
Touch HID, USB audio, partial updates, and a product-owned VID/PID remain out
of scope.

### No-Serial-Link Diagnostics

The placeholder frame uses its bottom status strip to show USB display state:

- yellow: device ready or USB disconnected
- cyan: USB mounted and waiting for a frame from Windows
- green: received USB Vendor data and assembling a frame
- red: received frame header or payload was rejected
- magenta: JPEG decode or AMOLED refresh failed

The strip is replaced by the Windows desktop after the first frame is decoded
and drawn successfully.

## Key Files

- `main/main.cpp`: app entry, startup logs, display init, placeholder frame
- `components/board_amoled_1_8/`: local board support component
- `sdkconfig.defaults`: default target and board-related configuration
