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
bool ntp_time_sync_is_syncing(void);
bool ntp_time_sync_is_due(void);
bool ntp_time_sync_is_retry_waiting(void);
uint32_t ntp_time_sync_retry_in_ms(void);
void ntp_time_sync_update(void);
time_t ntp_time_sync_last_sync_time(void);
bool ntp_time_sync_take_rtc_write_pending(void);

#ifdef __cplusplus
}
#endif
