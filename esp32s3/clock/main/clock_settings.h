#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define CLOCK_ALARM_COUNT 3

typedef struct {
    bool enabled;
    uint8_t hour;
    uint8_t minute;
    uint8_t weekdays; // bit0 Sunday ... bit6 Saturday; 0 = once
    uint8_t sound;    // 0 chime, 1 beep, 2 melody
} clock_alarm_t;

typedef struct {
    int brightness_percent;
    uint32_t sleep_timeout_sec;
    bool auto_rotate;
    bool wifi_enabled;
    bool ntp_enabled;
    uint8_t theme_index;
    bool sound_enabled;
    uint8_t sound_volume;
    bool ui_sounds;
    bool hourly_chime;
    clock_alarm_t alarms[CLOCK_ALARM_COUNT];
} clock_settings_t;

void clock_settings_defaults(clock_settings_t *settings);
esp_err_t clock_settings_load(clock_settings_t *settings);
esp_err_t clock_settings_save(const clock_settings_t *settings);

