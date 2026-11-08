#include "ntp_time_sync.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "portmacro.h"
#include "sdkconfig.h"

static const char *TAG = "NtpTimeSync";
static constexpr uint64_t NTP_SYNC_TIMEOUT_US = 45ULL * 1000ULL * 1000ULL;
static constexpr uint32_t NTP_SYNC_INTERVAL_MS = 2U * 60U * 60U * 1000U;
static constexpr uint32_t NTP_SYNC_INTERVAL_SEC = NTP_SYNC_INTERVAL_MS / 1000U;
static constexpr uint64_t NTP_RETRY_DELAYS_US[] = {
    5ULL * 60ULL * 1000ULL * 1000ULL,
    15ULL * 60ULL * 1000ULL * 1000ULL,
    30ULL * 60ULL * 1000ULL * 1000ULL,
    60ULL * 60ULL * 1000ULL * 1000ULL,
};
static constexpr uint8_t NTP_RETRY_DELAY_COUNT =
    static_cast<uint8_t>(sizeof(NTP_RETRY_DELAYS_US) / sizeof(NTP_RETRY_DELAYS_US[0]));
static constexpr const char *NTP_SERVERS[] = {
    "ntp.aliyun.com",
    "ntp.tencent.com",
    "cn.pool.ntp.org",
};
static constexpr size_t NTP_SERVER_COUNT = sizeof(NTP_SERVERS) / sizeof(NTP_SERVERS[0]);
static_assert(NTP_SERVER_COUNT <= CONFIG_LWIP_SNTP_MAX_SERVERS,
              "NTP server list exceeds CONFIG_LWIP_SNTP_MAX_SERVERS");

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized = false;
static bool s_running = false;
static bool s_synced = false;
static bool s_syncing = false;
static uint64_t s_sync_started_us = 0;
static uint8_t s_retry_index = 0;
static uint64_t s_retry_due_us = 0;
static bool s_rtc_write_pending = false;
static time_t s_last_sync_time = 0;

static void on_time_synced(struct timeval *tv)
{
    (void)tv;

    const time_t now = time(nullptr);
    portENTER_CRITICAL(&s_lock);
    s_synced = true;
    s_syncing = false;
    s_sync_started_us = 0;
    s_retry_index = 0;
    s_retry_due_us = 0;
    s_last_sync_time = now;
    // The callback runs in the lwIP/SNTP task. Defer the I2C RTC write to the
    // LVGL timer so it does not block the TCP/IP stack and stays serialized
    // with the other BSP I2C users.
    s_rtc_write_pending = true;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "SNTP time synchronized");
}

esp_err_t ntp_time_sync_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_set_sync_interval(NTP_SYNC_INTERVAL_MS);
    for (size_t i = 0; i < NTP_SERVER_COUNT; ++i) {
        esp_sntp_setservername((uint8_t)i, NTP_SERVERS[i]);
        ESP_LOGI(TAG, "SNTP server[%u]=%s", (unsigned)i, NTP_SERVERS[i]);
    }
    esp_sntp_set_time_sync_notification_cb(on_time_synced);
    ESP_LOGI(TAG, "SNTP sync interval: %u s", (unsigned)NTP_SYNC_INTERVAL_SEC);
    s_initialized = true;
    return ESP_OK;
}

esp_err_t ntp_time_sync_start(void)
{
    esp_err_t err = ntp_time_sync_init();
    if (err != ESP_OK) {
        return err;
    }

    portENTER_CRITICAL(&s_lock);
    const bool running = s_running;
    portEXIT_CRITICAL(&s_lock);
    if (running) {
        return ESP_OK;
    }

    esp_sntp_init();
    const uint64_t started_us = (uint64_t)esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_running = true;
    s_syncing = true;
    s_sync_started_us = started_us;
    s_retry_due_us = 0;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "SNTP started, primary=%s", NTP_SERVERS[0]);
    return ESP_OK;
}

void ntp_time_sync_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool running = s_running;
    s_running = false;
    s_syncing = false;
    s_sync_started_us = 0;
    s_retry_index = 0;
    s_retry_due_us = 0;
    portEXIT_CRITICAL(&s_lock);

    if (running) {
        esp_sntp_stop();
        ESP_LOGI(TAG, "SNTP stopped");
    }
}

esp_err_t ntp_time_sync_sync_now(void)
{
    esp_err_t err = ntp_time_sync_init();
    if (err != ESP_OK) {
        return err;
    }

    portENTER_CRITICAL(&s_lock);
    s_retry_index = 0;
    s_retry_due_us = 0;
    const bool running = s_running;
    portEXIT_CRITICAL(&s_lock);

    if (!running) {
        return ntp_time_sync_start();
    }

    const uint64_t started_us = (uint64_t)esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_syncing = true;
    s_sync_started_us = started_us;
    portEXIT_CRITICAL(&s_lock);

    if (!esp_sntp_restart()) {
        // The initial esp_sntp_init() may still be queued in the lwIP task.
        // Posting init again is harmless if SNTP is already running and starts
        // it if this is the first manual sync after boot.
        esp_sntp_init();
        ESP_LOGI(TAG, "SNTP init requested for manual sync");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "SNTP manual sync requested");
    return ESP_OK;
}

bool ntp_time_sync_is_running(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool running = s_running;
    portEXIT_CRITICAL(&s_lock);
    return running;
}

bool ntp_time_sync_is_synced(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool synced = s_synced;
    portEXIT_CRITICAL(&s_lock);
    return synced;
}

bool ntp_time_sync_is_due(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool synced = s_synced;
    const time_t last_sync = s_last_sync_time;
    portEXIT_CRITICAL(&s_lock);

    if (!synced || last_sync <= 0) {
        return true;
    }

    const time_t now = time(nullptr);
    if (now <= last_sync) {
        // The system clock moved backwards or is not trustworthy; sync again.
        return true;
    }

    return (uint64_t)(now - last_sync) >= NTP_SYNC_INTERVAL_SEC;
}

bool ntp_time_sync_is_syncing(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool syncing = s_syncing;
    portEXIT_CRITICAL(&s_lock);
    return syncing;
}

bool ntp_time_sync_is_retry_waiting(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool waiting = s_retry_due_us != 0;
    portEXIT_CRITICAL(&s_lock);
    return waiting;
}

uint32_t ntp_time_sync_retry_in_ms(void)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    const uint64_t due_us = s_retry_due_us;
    portEXIT_CRITICAL(&s_lock);

    if (due_us <= now_us) {
        return 0;
    }
    const uint64_t remaining_ms = (due_us - now_us) / 1000ULL;
    return remaining_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining_ms;
}

void ntp_time_sync_update(void)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    bool timed_out = false;
    bool was_running = false;
    uint64_t retry_delay_us = 0;

    portENTER_CRITICAL(&s_lock);
    if (s_retry_due_us != 0 && now_us >= s_retry_due_us) {
        s_retry_due_us = 0;
    }

    if (s_syncing && s_sync_started_us != 0 &&
        (now_us - s_sync_started_us) >= NTP_SYNC_TIMEOUT_US) {
        s_syncing = false;
        s_sync_started_us = 0;

        uint8_t index = s_retry_index;
        if (index >= NTP_RETRY_DELAY_COUNT) {
            index = NTP_RETRY_DELAY_COUNT - 1;
        }
        retry_delay_us = NTP_RETRY_DELAYS_US[index];
        s_retry_due_us = now_us + retry_delay_us;
        if (s_retry_index < NTP_RETRY_DELAY_COUNT - 1) {
            ++s_retry_index;
        }

        was_running = s_running;
        s_running = false;
        timed_out = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (timed_out) {
        if (was_running) {
            esp_sntp_stop();
        }
        ESP_LOGW(TAG,
                 "SNTP sync timed out after %u s, retry in %u s",
                 (unsigned)(NTP_SYNC_TIMEOUT_US / 1000000ULL),
                 (unsigned)(retry_delay_us / 1000000ULL));
    }
}

time_t ntp_time_sync_last_sync_time(void)
{
    portENTER_CRITICAL(&s_lock);
    const time_t last_sync = s_last_sync_time;
    portEXIT_CRITICAL(&s_lock);
    return last_sync;
}

bool ntp_time_sync_take_rtc_write_pending(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool pending = s_rtc_write_pending;
    s_rtc_write_pending = false;
    portEXIT_CRITICAL(&s_lock);
    return pending;
}
