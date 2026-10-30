#pragma once

#include "esp_err.h"

esp_err_t rtc_time_sync_from_external_rtc(void);
esp_err_t rtc_time_sync_write_from_system(void);
