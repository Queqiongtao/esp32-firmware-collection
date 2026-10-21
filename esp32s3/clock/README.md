# esp-clock

ESP32-S3 clock firmware for the Waveshare ESP32-S3-Touch-AMOLED-1.75 board.

This project is a clock application, not the Brookesia phone demo. The UI is built with LVGL v9 and rendered as a theme snapshot that is rotated by IMU-derived orientation.

See [PROJECT_OVERVIEW.md](PROJECT_OVERVIEW.md) for the detailed Chinese project guide.

## Features

- 466 x 466 AMOLED clock UI with switchable themes
- Switchable themes: minimal, segment, analog, outline analog, and four M5 WatchFace themes
- Current theme index is persisted in NVS and restored on boot
- Theme changes use a short scale/fade transition; rapid swipes are queued instead of dropped
- Settings menu opens on the brightness page, closes on a tap outside its content or on sleep, and keeps WiFi visibly disabled (`Not available`)
- Sleep and wake use a short brightness fade
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
- A normal horizontal swipe needs at least 100 px. A quick flick needs at least 50 px, must finish within 300 ms, and must have movement within the last 80 ms before release. The horizontal/vertical axis ratio is 2.0, and theme switches have a 150 ms cooldown.
- A roughly 2-second stationary long press opens settings; moving more than 16 px cancels long press and leaves the gesture available for swipe handling.
- The settings menu opens on the brightness page. Tapping outside its content or putting the screen to sleep closes it. The WiFi entry is disabled and shows `Not available`; WiFi connection is not implemented yet.
- Sleep and wake use a roughly 200 ms brightness fade. The first touch that wakes the screen only turns it on; it does not switch themes or activate menu items.
- The rotation pipeline reuses one RGB565 snapshot buffer and refreshes it only when visible theme content changes. Theme transitions only change image scale and opacity, so they do not allocate another full-screen buffer.
- The theme index is stored as `theme_id` in the NVS `clock_settings` namespace. It maps to the `s_themes` array order, so adding or reordering themes remaps saved values.

## Key Files

- `main/main.cpp`: app entry, theme switching and transition, LVGL snapshot, rotation spring, brightness fade, GPIO0/touch/power-key wiring
- `main/imu_demo.cpp`: accelerometer sampling and clock angle update
- `main/rtc_time_sync.cpp`: PCF85063 time sync with build-time fallback
- `main/clock_settings.cpp` / `main/clock_settings.h`: NVS settings (brightness, sleep timeout, WiFi flag, theme index)
- `main/clock_settings_menu.cpp` / `main/clock_settings_menu.h`: settings menu UI
- `main/clock_touch_gesture.cpp` / `main/clock_touch_gesture.h`: touch swipe/flick and long-press gesture layer
- `main/clock_theme.h`: theme interface
- `main/clock_theme_minimal.cpp`
- `main/lv_font_clock_144.c`: 144 px clock font used by the minimal theme
- `main/clock_theme_segment.cpp`
- `main/clock_theme_analog.cpp`
- `main/clock_theme_outline_analog.cpp`
- `components/qmi8658/`: QMI8658 I2C driver
- `components/pcf85063/`: PCF85063 I2C driver
- `components/ui_physics/`: angle spring smoothing
- `tools/preview/`: host-side theme renderer using the LVGL desktop build
- `doc/theme-optimization-plan.md`: theme roadmap, progress, and design notes
