# esp-clock

ESP32-S3 clock firmware for the Waveshare ESP32-S3-Touch-AMOLED-1.75 board.

This project is a clock application, not the Brookesia phone demo. The UI is built with LVGL v9 and rendered as a theme snapshot that can optionally be rotated by IMU-derived orientation; auto-rotate is off by default.

See [PROJECT_OVERVIEW.md](PROJECT_OVERVIEW.md) for the detailed Chinese project guide.

## Features

- 466 x 466 AMOLED clock UI with switchable themes
- Switchable themes: minimal, segment, analog, outline analog, and four M5 WatchFace themes
- Current theme index is persisted in NVS and restored on boot
- Theme changes use a short scale/fade transition; rapid swipes are queued instead of dropped
- Settings menu opens on the brightness page and closes via the top-center Done button or after 1 minute without menu activity; the WiFi page supports on-screen SSID/password entry and NTP time sync
- Motion settings page provides an Auto Rotate switch; it is off by default and pauses IMU sampling when disabled
- Sleep and wake use a short brightness fade
- Current theme is snapshotted into a reusable RGB565 buffer and rotated as an image when auto-rotate is enabled
- PCF85063 external RTC syncs system time, with build time as fallback; NTP updates the system time and writes it back to the RTC after WiFi connects
- QMI8658 IMU computes in-plane gravity angle for optional clock rotation
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
- WiFi: 2.4 GHz (ESP32-S3)

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

It compiles the same `clock_theme_*.cpp` sources and settings menu with a desktop LVGL build and renders themes to `tools/preview/out/` plus menu pages to `tools/preview/out/menu/` as PNG. It is an approximation of the firmware renderer, not the firmware itself: the entry point, LVGL configuration, time source, display backend and snapshot target differ. See `tools/preview/README.md` for usage and `doc/preview-tool.md` for the design boundaries.

Build output:

- Firmware image: `build/esp-clock.bin`
- Partition table: custom layout with a 6 MB factory app partition

## Runtime Notes

- The IMU task samples QMI8658 acceleration at 100 Hz and keeps sampling paused while the menu is open, the screen is asleep, or auto-rotate is disabled.
- GPIO0 is also a boot-related pin; holding it during power-on or reset can affect boot mode.
- A normal horizontal swipe needs at least 100 px. A quick flick needs at least 50 px, must finish within 300 ms, and must have movement within the last 80 ms before release. The horizontal/vertical axis ratio is 2.0, and theme switches have a 150 ms cooldown.
- A roughly 2-second stationary long press opens settings; moving more than 16 px cancels long press and leaves the gesture available for swipe handling.
- The settings menu opens on the brightness page and contains Brightness, Sleep, Motion and WiFi pages. Use the four icon-and-label tabs arranged on a shallow arc at the bottom to switch pages; Done is centered at the top. Tapping outside the menu no longer closes it; use Done, wait 1 minute without menu activity, or let the screen sleep. Auto Rotate on the Motion page is off by default.
- The WiFi page has an enable switch, a Setup button, an Auto NTP switch, a Sync now button and two status lines. When connected, the detail line shows IP, RSSI and NTP state; failures show a short reason such as `Wrong password` or `Network not found`.
- Setup opens a summary overlay with separate WiFi Name and Password rows. Tapping a row opens a single-field editor with a larger keyboard, a mint focus border and a password show/hide button. Done, Back and the keyboard back action keep the current draft; Connect saves it and stays on the summary with an error if saving or starting WiFi fails. Credentials are stored in the NVS `wifi_creds` namespace in plaintext; editing only the SSID keeps the saved password. NVS namespace/key names are limited to 15 characters. The menu timeout is paused while the setup overlay is open.
- After WiFi connects, SNTP starts automatically with `ntp.aliyun.com`, `ntp.tencent.com` and `cn.pool.ntp.org` as fallback servers and the synchronized time is written back to the PCF85063. Auto NTP can be disabled; Sync now remains available while WiFi is connected. A manual sync keeps SNTP alive for up to 30 seconds when Auto NTP is off.
- Sleep and wake use a roughly 200 ms brightness fade. The first touch that wakes the screen only turns it on; it does not switch themes or activate menu items.
- The rotation pipeline reuses one RGB565 snapshot buffer and refreshes it only when visible theme content changes. Theme transitions only change image scale and opacity, so they do not allocate another full-screen buffer.
- The theme index is stored as `theme_id` in the NVS `clock_settings` namespace. It maps to the `s_themes` array order, so adding or reordering themes remaps saved values.
- Auto-rotate is stored as `auto_rotate` in the same NVS namespace. It defaults to off, and existing devices without the key stay off after upgrading.
- The WiFi enable flag is stored as `wifi` and NTP auto-sync as `ntp` in the `clock_settings` namespace; `ntp` defaults to on for devices upgrading without the key. If WiFi is enabled on boot and credentials exist, the device connects automatically and retries every 5 seconds after a disconnect. The WiFi page reports `WiFi off` whenever the switch is off, even if saved credentials exist.

## Key Files

- `main/main.cpp`: app entry, theme switching and transition, LVGL snapshot, rotation spring, brightness fade, GPIO0/touch/power-key wiring
- `main/imu_demo.cpp`: accelerometer sampling and clock angle update
- `main/rtc_time_sync.cpp`: PCF85063 time sync with build-time fallback, plus system-time write-back after NTP sync
- `main/wifi_manager.cpp` / `main/wifi_manager.h`: WiFi station connection, NVS credentials and reconnect handling
- `main/ntp_time_sync.cpp` / `main/ntp_time_sync.h`: SNTP start/stop, sync status and manual sync
- `main/clock_settings.cpp` / `main/clock_settings.h`: NVS settings (brightness, sleep timeout, auto-rotate, WiFi flag, NTP flag, theme index)
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
- `tools/preview/`: host-side theme renderer using the same theme sources and the LVGL desktop build (an approximation, not the firmware)
- `doc/preview-tool.md`: preview design, firmware differences and maintenance rules
- `doc/theme-optimization-plan.md`: theme roadmap, progress, and design notes
