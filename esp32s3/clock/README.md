# esp-clock

ESP32-S3 clock firmware for the Waveshare ESP32-S3-Touch-AMOLED-1.75 board.

This project is a clock application, not the Brookesia phone demo. The UI is built with LVGL v9 and rendered as a theme snapshot that is rotated by IMU-derived orientation.

See [PROJECT_OVERVIEW.md](PROJECT_OVERVIEW.md) for the detailed Chinese project guide.

## Features

- 466 x 466 AMOLED clock UI with switchable themes
- Switchable themes: minimal, segment, analog, outline analog, and four M5 WatchFace themes
- Current theme is snapshotted into a reusable RGB565 buffer and rotated as an image
- PCF85063 external RTC syncs system time, with build time as fallback
- QMI8658 IMU computes in-plane gravity angle for clock rotation
- GPIO0 and touch swipes switch themes
- Theme switch order and implementation details are documented in `PROJECT_OVERVIEW.md`

## Hardware

- Board: Waveshare ESP32-S3-Touch-AMOLED-1.75
- Chip: ESP32-S3
- Flash: 16 MB
- PSRAM: 8 MB
- Display: 466 x 466 AMOLED
- RTC: PCF85063
- IMU: QMI8658
- Touch: CST9217

## Commands

Use the root helper script instead of assuming ESP-IDF is manually activated:

```bash
./dev.sh build
./dev.sh ports
./dev.sh flash-monitor
./dev.sh -p /dev/ttyUSB0 flash-monitor
```

Theme preview runs on the host without flashing:

```bash
./tools/preview/run.sh
```

It renders every theme to `tools/preview/out/` as PNG; see `tools/preview/README.md` for details.

Build output:

- Firmware image: `build/esp-clock.bin`
- Partition table: custom layout with a 6 MB factory app partition

## Runtime Notes

- The IMU task samples QMI8658 acceleration at 100 Hz and keeps sampling paused while the menu is open or the screen is asleep.
- GPIO0 is also a boot-related pin; holding it during power-on or reset can affect boot mode.
- Touch settings require a roughly 2-second stationary long press; moving more than 16 px cancels long press and leaves the gesture available for swipe handling.
- The rotation pipeline reuses one RGB565 snapshot buffer and refreshes it only when visible theme content changes.

## Key Files

- `main/main.cpp`: app entry, theme switching, LVGL snapshot, rotation spring, GPIO0 button wiring
- `main/imu_demo.cpp`: accelerometer sampling and clock angle update
- `main/rtc_time_sync.cpp`: PCF85063 time sync with build-time fallback
- `main/clock_theme.h`: theme interface
- `main/clock_theme_minimal.cpp`
- `main/clock_theme_segment.cpp`
- `main/clock_theme_analog.cpp`
- `main/clock_theme_outline_analog.cpp`
- `components/qmi8658/`: QMI8658 I2C driver
- `components/pcf85063/`: PCF85063 I2C driver
- `components/ui_physics/`: angle spring smoothing
- `tools/preview/`: host-side theme renderer using the LVGL desktop build
- `doc/theme-optimization-plan.md`: theme roadmap, progress, and design notes
