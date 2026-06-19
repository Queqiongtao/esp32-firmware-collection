#include "rtc_time_sync.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "pcf85063.h"

static const char *TAG = "RtcTimeSync";

static bool is_leap_year(int year)
{
    return ((year % 4) == 0 && (year % 100) != 0) || ((year % 400) == 0);
}

static int days_in_month(int year, int month)
{
    static const int days[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31,
    };

    if (month == 2 && is_leap_year(year)) {
        return 29;
    }

    return days[month - 1];
}

static bool is_app_time_sane(const struct tm *timeinfo)
{
    const int year = timeinfo->tm_year + 1900;
    const int month = timeinfo->tm_mon + 1;
    const int day = timeinfo->tm_mday;

    if (year < 2024 || year > 2099) {
        return false;
    }
    if (month < 1 || month > 12) {
        return false;
    }
    if (day < 1 || day > days_in_month(year, month)) {
        return false;
    }
    if (timeinfo->tm_hour < 0 || timeinfo->tm_hour > 23 ||
        timeinfo->tm_min < 0 || timeinfo->tm_min > 59 ||
        timeinfo->tm_sec < 0 || timeinfo->tm_sec > 59) {
        return false;
    }

    return true;
}

static bool parse_build_time(struct tm *timeinfo)
{
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };

    char month_text[4] = {};
    int day = 0;
    int year = 0;
    int hour = 0;
    int min = 0;
    int sec = 0;

    if (sscanf(__DATE__, "%3s %d %d", month_text, &day, &year) != 3 ||
        sscanf(__TIME__, "%d:%d:%d", &hour, &min, &sec) != 3) {
        return false;
    }

    int month = 0;
    for (int i = 0; i < 12; ++i) {
        if (strcmp(month_text, months[i]) == 0) {
            month = i + 1;
            break;
        }
    }

    if (month == 0) {
        return false;
    }

    memset(timeinfo, 0, sizeof(*timeinfo));
    timeinfo->tm_year = year - 1900;
    timeinfo->tm_mon = month - 1;
    timeinfo->tm_mday = day;
    timeinfo->tm_hour = hour;
    timeinfo->tm_min = min;
    timeinfo->tm_sec = sec;
    timeinfo->tm_isdst = -1;

    return is_app_time_sane(timeinfo);
}

static esp_err_t set_system_time_from_tm(const struct tm *timeinfo)
{
    struct tm normalized_time = *timeinfo;
    time_t now = mktime(&normalized_time);
    if (now < 0) {
        return ESP_FAIL;
    }

    const struct timeval tv = {
        .tv_sec = now,
        .tv_usec = 0,
    };

    return settimeofday(&tv, NULL) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t init_pcf85063(pcf85063_t *rtc)
{
    i2c_master_bus_handle_t bus_handle = bsp_i2c_get_handle();
    if (bus_handle == NULL) {
        return ESP_FAIL;
    }

    const pcf85063_config_t config = {
        .bus_handle = bus_handle,
        .device_address = PCF85063_DEFAULT_I2C_ADDRESS,
        .scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ,
        .timeout_ms = PCF85063_DEFAULT_TIMEOUT_MS,
    };

    return pcf85063_init(rtc, &config);
}

esp_err_t rtc_time_sync_from_external_rtc(void)
{
    pcf85063_t rtc = {};
    esp_err_t err = init_pcf85063(&rtc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to init PCF85063: %s", esp_err_to_name(err));
        return err;
    }

    struct tm timeinfo = {};
    err = pcf85063_get_time(&rtc, &timeinfo);
    if (err == ESP_OK && is_app_time_sane(&timeinfo)) {
        err = set_system_time_from_tm(&timeinfo);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "System time synced from PCF85063 RTC");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Failed to set system time from RTC: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "PCF85063 RTC time invalid/unavailable: %s", esp_err_to_name(err));
    }

    if (!parse_build_time(&timeinfo)) {
        ESP_LOGW(TAG, "Failed to parse firmware build time");
        return ESP_FAIL;
    }

    if (mktime(&timeinfo) < 0) {
        ESP_LOGW(TAG, "Failed to normalize firmware build time");
        return ESP_FAIL;
    }

    err = pcf85063_set_time(&rtc, &timeinfo);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize PCF85063 from build time: %s", esp_err_to_name(err));
    }

    err = set_system_time_from_tm(&timeinfo);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "System time initialized from firmware build time");
    } else {
        ESP_LOGW(TAG, "Failed to set system time from build time: %s", esp_err_to_name(err));
    }

    return err;
}
