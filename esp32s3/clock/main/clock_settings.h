#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int brightness_percent;
    uint32_t sleep_timeout_sec;
    bool wifi_enabled;
} clock_settings_t;

void clock_settings_defaults(clock_settings_t *settings);
esp_err_t clock_settings_load(clock_settings_t *settings);
esp_err_t clock_settings_save(const clock_settings_t *settings);

