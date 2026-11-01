# esp-clock

ESP32-S3 clock firmware for the Waveshare ESP32-S3-Touch-AMOLED-1.75 board.

This project is a clock application, not the Brookesia phone demo. The UI is built with LVGL v9 and rendered as a theme snapshot that can optionally be rotated by IMU-derived orientation; auto-rotate is off by default.

See [PROJECT_OVERVIEW.md](PROJECT_OVERVIEW.md) for the detailed Chinese project guide.

## Features

- 466 x 466 AMOLED clock UI with switchable themes
- Switchable themes: minimal, segment, analog, dot matrix, and four M5 WatchFace themes
- Current theme index is persisted in NVS and restored on boot
- Theme changes use a short scale/fade transition; rapid swipes are queued instead of dropped
- Settings menu opens on the brightness page and closes via the top-center Done button or after 1 minute without menu activity; the WiFi page supports on-screen SSID/password entry, nearby AP scanning and NTP time sync
- Motion settings page provides an Auto Rotate switch; it is off by default and pauses IMU sampling when disabled
- Sleep and wake use a short brightness fade
- Current theme is snapshotted into a reusable RGB565 buffer and rotated as an image when auto-rotate is enabled
- PCF85063 external RTC syncs system time, with build time as fallback; NTP updates the system time and writes it back to the RTC after WiFi connects
- ES8311 codec and a GPIO46 power amplifier play synthesized UI sounds, hourly chimes, up to three alarms, timers and USB-only sleep noise
- AXP2101 manages 3.7 V battery charging and power-path control; the Power page shows a clickable battery card and a Battery detail overlay with percentage, voltage and charging state. When the E-Gauge is unavailable, the percentage is estimated from battery voltage and marked with a `~` prefix.
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
- Power management: AXP2101 with 3.7 V single-cell LiPo charging
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
- The settings menu opens on the brightness page and contains Brightness, Sleep, Motion, WiFi and Sound pages. Use the five icon-and-label tabs arranged on a shallow arc at the bottom to switch pages; Done is centered at the top. Tapping outside the menu no longer closes it; use Done, wait 1 minute without menu activity, or let the screen sleep. Auto Rotate on the Motion page is off by default.
- The WiFi page has an enable switch, a Setup button, an Auto NTP switch, a Sync now button and two status lines. When connected, the detail line shows IP, RSSI and NTP state; failures show a short reason such as `Wrong password` or `Network not found`.
- Setup opens a summary overlay with separate WiFi Name and Password rows and a Scan networks button that lists nearby APs; selecting an SSID goes straight to password entry, while hidden networks can still be typed manually. Tapping a row opens a single-field editor with a 6-column 350 × 270 keyboard, larger 58 × 54 px keys, key popovers, case/symbol layers, a mint focus border and a password show/hide button. Done and Back keep the current draft; Connect saves it and stays on the summary with an error if saving or starting WiFi fails. Credentials are stored in the NVS `wifi_creds` namespace in plaintext; editing only the SSID keeps the saved password. NVS namespace/key names are limited to 15 characters. The menu timeout is paused while the setup overlay is open.
- Scan networks calls `esp_wifi_scan_start()` synchronously from the LVGL event. The UI shows `Scanning...` for roughly 1–3 seconds and then lists up to 8 APs. If WiFi was off, the STA is started temporarily and stopped after the scan; hidden networks can still be entered manually.
- After WiFi connects, SNTP starts automatically with `ntp.aliyun.com`, `ntp.tencent.com` and `cn.pool.ntp.org` as fallback servers and the synchronized time is written back to the PCF85063. Auto NTP can be disabled; Sync now remains available while WiFi is connected. A manual sync keeps SNTP alive for up to 30 seconds when Auto NTP is off.
- Sleep and wake use a roughly 200 ms brightness fade. The first touch that wakes the screen only turns it on; it does not switch themes or activate menu items. Touch/PWR wake and manual/auto sleep play short confirmation tones when UI sounds are enabled. PWR uses the AXP2101 PKEY IRQ as a battery-only fallback in addition to EXIO4.
- The rotation pipeline reuses one RGB565 snapshot buffer and refreshes it only when visible theme content changes. Theme transitions only change image scale and opacity, so they do not allocate another full-screen buffer.
- Sound settings are stored as `sound_on`, `sound_vol`, `ui_sound` and `chime_on`; the three alarms use the `alarm_cfg` blob. Sleep noise is USB-only and stops when USB is removed.
- The theme index is stored as `theme_id` in the NVS `clock_settings` namespace. It maps to the `s_themes` array order. After removing the outline theme, a missing `theme_map` key triggers a one-time migration: old index 3 falls back to analog, and old indices 4–7 shift down to 3–6.
- Auto-rotate is stored as `auto_rotate` in the same NVS namespace. It defaults to off, and existing devices without the key stay off after upgrading.
- M5 WatchFace fonts are regenerated at 4bpp from the upstream Commissioner and Maple Mono TTFs; the original 2bpp assets look soft at 108 px and 80 px. `m5_number_flow` intentionally uses 80 px digits and a 28 px date instead of the upstream 64/24. Regenerate with `./tools/fonts/generate_m5_fonts.sh`; see `tools/fonts/README.md`.
- The Sound page controls the master switch, volume, a Test overlay for auditioning each sound, UI sounds and hourly chime, and opens Alarms, Timer and Sleep overlays. Alarms support up to three entries with weekday repeat, sound choice and snooze/stop; Timer supports presets and Pomodoro 25/5; Sleep plays generated white/brown noise for 15/30/60 minutes only while USB-powered. All audio is synthesized in firmware; no SD card or audio files are required.
- The Power page battery card opens a Battery detail overlay with large percentage, voltage, USB state and charge configuration. AXP2101 uses the shared I2C bus at address `0x34`. TS pin temperature measurement is disabled, and charging is configured for 50 mA precharge, 400 mA constant current, 25 mA termination and 4.2 V target. The E-Gauge is explicitly enabled (REG18h bit3), but register `0xA4` may stay at 0 on this board; in that case the firmware estimates the percentage from a 3300–4200 mV curve (with 70 mV charging compensation), marks the value with `~`, and shows `Estimated | 4.2 V / 400 mA` in the detail overlay. EXIO4 is active-high for PWR; the AXP2101 PKEY positive-edge IRQ is also polled as a battery-only fallback, and duplicate EXIO4/AXP events are suppressed. The first release after battery power-on is ignored to avoid immediately sleeping the screen.
- The WiFi enable flag is stored as `wifi` and NTP auto-sync as `ntp` in the `clock_settings` namespace; `ntp` defaults to on for devices upgrading without the key. If WiFi is enabled on boot and credentials exist, the device connects automatically and retries every 5 seconds after a disconnect. The WiFi page reports `WiFi off` whenever the switch is off, even if saved credentials exist.

## Key Files

- `main/main.cpp`: app entry, theme switching and transition, LVGL snapshot, rotation spring, brightness fade, GPIO0/touch/power-key wiring
- `main/imu_demo.cpp`: accelerometer sampling and clock angle update
- `main/rtc_time_sync.cpp`: PCF85063 time sync with build-time fallback, plus system-time write-back after NTP sync
- `main/wifi_manager.cpp` / `main/wifi_manager.h`: WiFi station connection, NVS credentials, AP scanning and reconnect handling
- `main/ntp_time_sync.cpp` / `main/ntp_time_sync.h`: SNTP start/stop, sync status and manual sync
- `main/power_manager.cpp` / `main/power_manager.h`: AXP2101 charging configuration, battery telemetry and voltage-based percentage fallback
- `main/audio_manager.cpp` / `main/audio_manager.h`: ES8311/PA audio task, synthesized sounds, alarms and sleep noise
- `main/clock_settings.cpp` / `main/clock_settings.h`: NVS settings (brightness, sleep timeout, auto-rotate, WiFi flag, NTP flag, theme index, sound settings and three alarms)
- `main/clock_settings_menu.cpp` / `main/clock_settings_menu.h`: settings menu UI (five tabs, WiFi setup/AP scan/keyboard, Sound/Alarm/Timer/Sleep overlays and NTP controls)
- `main/clock_touch_gesture.cpp` / `main/clock_touch_gesture.h`: touch swipe/flick and long-press gesture layer
- `main/clock_theme.h`: theme interface
- `main/clock_theme_minimal.cpp`
- `main/lv_font_clock_144.c`: 144 px clock font used by the minimal theme
- `main/m5_assets/fonts/`: Commissioner and Maple Mono fonts regenerated at 4bpp
- `tools/fonts/`: M5 font regeneration script and notes
- `main/clock_theme_segment.cpp`
- `main/clock_theme_analog.cpp`
- `main/clock_theme_dot_matrix.cpp`: 59×20 dot-matrix clock with 6 px tiles, Paytone One 12×20 glyphs and a 10-color candy palette; shows HH:MM with a `TUE 20 OCT • 35` info line, generous colon spacing and no background grid or scan animation
- `components/qmi8658/`: QMI8658 I2C driver
- `components/pcf85063/`: PCF85063 I2C driver
- `components/ui_physics/`: angle spring smoothing
- `tools/preview/`: host-side theme renderer using the same theme sources and the LVGL desktop build (an approximation, not the firmware)
- `doc/preview-tool.md`: preview design, firmware differences and maintenance rules
- `doc/theme-optimization-plan.md`: theme roadmap, progress, and design notes
