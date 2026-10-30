#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ntp_time_sync_init(void);
esp_err_t ntp_time_sync_start(void);
void ntp_time_sync_stop(void);
esp_err_t ntp_time_sync_sync_now(void);
bool ntp_time_sync_is_running(void);
bool ntp_time_sync_is_synced(void);
time_t ntp_time_sync_last_sync_time(void);
bool ntp_time_sync_take_rtc_write_pending(void);

#ifdef __cplusplus
}
#endif
