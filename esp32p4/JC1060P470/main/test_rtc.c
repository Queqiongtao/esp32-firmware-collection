/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "test_rtc.h"

#include <inttypes.h>
#include "display_touch.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "test_config.h"

static const char *TAG = "test_rtc";

#define RX8025_REG_SEC                           0x00
#define RX8025_REG_EXT                           0x0D
#define RX8025_TIME_REG_COUNT                    7
#define RX8025_STATUS_REG_COUNT                  3

#define RX8025_SEC_MASK                          0x7F
#define RX8025_MIN_MASK                          0x7F
#define RX8025_HOUR_MASK                         0x3F
#define RX8025_WDAY_MASK                         0x7F
#define RX8025_MDAY_MASK                         0x3F
#define RX8025_MONTH_MASK                        0x1F
#define RX8025_YEAR_BASE                         2000
#define RX8025_TASK_STACK_SIZE                   3072
#define RX8025_TASK_PRIORITY                     3

static i2c_master_dev_handle_t s_rtc_dev = NULL;
static TaskHandle_t s_rtc_task_handle = NULL;
static bool s_initialized = false;
static bool s_time_valid = false;
static bool s_synced_from_build_time = false;
static esp_err_t s_last_error = ESP_OK;

typedef struct {
    uint8_t sec;
    uint8_t min;
    uint8_t hour;
    uint8_t wday_mask;
    uint8_t mday;
    uint8_t month;
    uint16_t year;
} rtc_time_t;

static rtc_time_t s_last_time = {0};

static uint8_t rtc_bcd_to_dec(uint8_t val)
{
    return (uint8_t)(((val >> 4) * 10) + (val & 0x0F));
}

static uint8_t rtc_dec_to_bcd(uint8_t val)
{
    return (uint8_t)(((val / 10) << 4) | (val % 10));
}

static bool rtc_bcd_is_valid(uint8_t val, uint8_t max)
{
    return (val & 0x0F) <= 9 && rtc_bcd_to_dec(val) <= max;
}

static uint8_t rtc_weekday_mask(uint16_t year, uint8_t month, uint8_t mday)
{
    static const uint8_t month_offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    uint16_t y = year;

    if (month < 3) {
        y--;
    }

    uint8_t wday = (uint8_t)((y + y / 4 - y / 100 + y / 400 + month_offsets[month - 1] + mday) % 7);
    return (uint8_t)(1U << wday);
}

static int rtc_month_from_build_date(const char *month)
{
    static const char *month_names[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };

    for (int i = 0; i < 12; ++i) {
        if (month[0] == month_names[i][0] &&
            month[1] == month_names[i][1] &&
            month[2] == month_names[i][2]) {
            return i + 1;
        }
    }
    return -1;
}

static esp_err_t rtc_get_build_time(rtc_time_t *out_time)
{
    ESP_RETURN_ON_FALSE(out_time != NULL, ESP_ERR_INVALID_ARG, TAG, "out_time is NULL");

    const char *date = __DATE__;
    const char *time = __TIME__;
    int month = rtc_month_from_build_date(date);
    ESP_RETURN_ON_FALSE(month > 0, ESP_ERR_INVALID_RESPONSE, TAG, "parse build month failed: %s", date);

    int mday = ((date[4] == ' ') ? 0 : date[4] - '0') * 10 + (date[5] - '0');
    int year = (date[7] - '0') * 1000 + (date[8] - '0') * 100 + (date[9] - '0') * 10 + (date[10] - '0');
    int hour = (time[0] - '0') * 10 + (time[1] - '0');
    int min = (time[3] - '0') * 10 + (time[4] - '0');
    int sec = (time[6] - '0') * 10 + (time[7] - '0');

    ESP_RETURN_ON_FALSE(year >= RX8025_YEAR_BASE && year <= RX8025_YEAR_BASE + 99,
                        ESP_ERR_INVALID_RESPONSE, TAG, "build year out of RTC range: %d", year);
    ESP_RETURN_ON_FALSE(mday >= 1 && mday <= 31, ESP_ERR_INVALID_RESPONSE, TAG, "build day invalid: %d", mday);

    out_time->sec = (uint8_t)sec;
    out_time->min = (uint8_t)min;
    out_time->hour = (uint8_t)hour;
    out_time->mday = (uint8_t)mday;
    out_time->month = (uint8_t)month;
    out_time->year = (uint16_t)year;
    out_time->wday_mask = rtc_weekday_mask(out_time->year, out_time->month, out_time->mday);
    return ESP_OK;
}

static esp_err_t rtc_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(s_rtc_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "RTC not initialized");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "data is NULL");

    return i2c_master_transmit_receive(s_rtc_dev,
                                       &reg,
                                       sizeof(reg),
                                       data,
                                       len,
                                       TEST_RTC_I2C_TIMEOUT_MS);
}

static esp_err_t rtc_write_time(const rtc_time_t *time)
{
    ESP_RETURN_ON_FALSE(s_rtc_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "RTC not initialized");
    ESP_RETURN_ON_FALSE(time != NULL, ESP_ERR_INVALID_ARG, TAG, "time is NULL");
    ESP_RETURN_ON_FALSE(time->year >= RX8025_YEAR_BASE && time->year <= RX8025_YEAR_BASE + 99,
                        ESP_ERR_INVALID_ARG, TAG, "year out of range: %" PRIu16, time->year);

    uint8_t write_buf[1 + RX8025_TIME_REG_COUNT] = {
        RX8025_REG_SEC,
        rtc_dec_to_bcd(time->sec),
        rtc_dec_to_bcd(time->min),
        rtc_dec_to_bcd(time->hour),
        time->wday_mask,
        rtc_dec_to_bcd(time->mday),
        rtc_dec_to_bcd(time->month),
        rtc_dec_to_bcd((uint8_t)(time->year - RX8025_YEAR_BASE)),
    };

    return i2c_master_transmit(s_rtc_dev, write_buf, sizeof(write_buf), TEST_RTC_I2C_TIMEOUT_MS);
}

static esp_err_t rtc_decode_time(const uint8_t *time_regs, rtc_time_t *out_time)
{
    ESP_RETURN_ON_FALSE(time_regs != NULL, ESP_ERR_INVALID_ARG, TAG, "time_regs is NULL");
    ESP_RETURN_ON_FALSE(out_time != NULL, ESP_ERR_INVALID_ARG, TAG, "out_time is NULL");

    const uint8_t sec_bcd = time_regs[0] & RX8025_SEC_MASK;
    const uint8_t min_bcd = time_regs[1] & RX8025_MIN_MASK;
    const uint8_t hour_bcd = time_regs[2] & RX8025_HOUR_MASK;
    const uint8_t wday_raw = time_regs[3] & RX8025_WDAY_MASK;
    const uint8_t mday_bcd = time_regs[4] & RX8025_MDAY_MASK;
    const uint8_t month_bcd = time_regs[5] & RX8025_MONTH_MASK;
    const uint8_t year_bcd = time_regs[6];

    ESP_RETURN_ON_FALSE(rtc_bcd_is_valid(sec_bcd, 59), ESP_ERR_INVALID_RESPONSE,
                        TAG, "invalid RTC seconds BCD 0x%02X", time_regs[0]);
    ESP_RETURN_ON_FALSE(rtc_bcd_is_valid(min_bcd, 59), ESP_ERR_INVALID_RESPONSE,
                        TAG, "invalid RTC minutes BCD 0x%02X", time_regs[1]);
    ESP_RETURN_ON_FALSE(rtc_bcd_is_valid(hour_bcd, 23), ESP_ERR_INVALID_RESPONSE,
                        TAG, "invalid RTC hours BCD 0x%02X", time_regs[2]);
    ESP_RETURN_ON_FALSE(wday_raw != 0, ESP_ERR_INVALID_RESPONSE,
                        TAG, "invalid RTC weekday mask 0x%02X", time_regs[3]);
    ESP_RETURN_ON_FALSE(rtc_bcd_is_valid(mday_bcd, 31), ESP_ERR_INVALID_RESPONSE,
                        TAG, "invalid RTC day BCD 0x%02X", time_regs[4]);
    ESP_RETURN_ON_FALSE(rtc_bcd_is_valid(month_bcd, 12), ESP_ERR_INVALID_RESPONSE,
                        TAG, "invalid RTC month BCD 0x%02X", time_regs[5]);
    ESP_RETURN_ON_FALSE(rtc_bcd_is_valid(year_bcd, 99), ESP_ERR_INVALID_RESPONSE,
                        TAG, "invalid RTC year BCD 0x%02X", time_regs[6]);

    out_time->sec = rtc_bcd_to_dec(sec_bcd);
    out_time->min = rtc_bcd_to_dec(min_bcd);
    out_time->hour = rtc_bcd_to_dec(hour_bcd);
    out_time->wday_mask = wday_raw;
    out_time->mday = rtc_bcd_to_dec(mday_bcd);
    out_time->month = rtc_bcd_to_dec(month_bcd);
    out_time->year = RX8025_YEAR_BASE + rtc_bcd_to_dec(year_bcd);

    ESP_RETURN_ON_FALSE(out_time->mday >= 1, ESP_ERR_INVALID_RESPONSE,
                        TAG, "invalid RTC day value %" PRIu8, out_time->mday);
    ESP_RETURN_ON_FALSE(out_time->month >= 1, ESP_ERR_INVALID_RESPONSE,
                        TAG, "invalid RTC month value %" PRIu8, out_time->month);
    return ESP_OK;
}

static void rtc_log_time(const char *prefix, const rtc_time_t *time)
{
    ESP_LOGI(TAG, "%s%s time: %04" PRIu16 "-%02" PRIu8 "-%02" PRIu8
             " %02" PRIu8 ":%02" PRIu8 ":%02" PRIu8 " wday_mask=0x%02X",
             prefix ? prefix : "",
             TEST_RTC_CHIP_NAME,
             time->year, time->month, time->mday, time->hour, time->min, time->sec, time->wday_mask);
}

static void rtc_store_time(const rtc_time_t *time, bool valid)
{
    if (time != NULL) {
        s_last_time = *time;
    }
    s_time_valid = valid;
}

static esp_err_t rtc_read_time_and_status(uint8_t *time_regs, uint8_t *status_regs)
{
    ESP_RETURN_ON_ERROR(rtc_read_regs(RX8025_REG_SEC, time_regs, RX8025_TIME_REG_COUNT),
                        TAG, "read RTC time registers failed");
    ESP_RETURN_ON_ERROR(rtc_read_regs(RX8025_REG_EXT, status_regs, RX8025_STATUS_REG_COUNT),
                        TAG, "read RTC status registers failed");

    ESP_LOG_BUFFER_HEXDUMP(TAG, time_regs, RX8025_TIME_REG_COUNT, ESP_LOG_INFO);
    ESP_LOGI(TAG, "status raw: ext=0x%02X flag=0x%02X ctrl=0x%02X",
             status_regs[0], status_regs[1], status_regs[2]);
    return ESP_OK;
}

static esp_err_t rtc_read_and_log_time(const char *prefix)
{
    uint8_t time_regs[RX8025_TIME_REG_COUNT] = {0};
    rtc_time_t rtc_time = {0};

    ESP_RETURN_ON_ERROR(rtc_read_regs(RX8025_REG_SEC, time_regs, RX8025_TIME_REG_COUNT),
                        TAG, "read RTC time registers failed");
    ESP_RETURN_ON_ERROR(rtc_decode_time(time_regs, &rtc_time), TAG, "decode RTC time failed");
    rtc_store_time(&rtc_time, true);
    rtc_log_time(prefix, &rtc_time);
    return ESP_OK;
}

static void rtc_periodic_task(void *arg)
{
    (void)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(TEST_RTC_READ_INTERVAL_MS));

        if (!s_initialized) {
            continue;
        }

        esp_err_t ret = rtc_read_and_log_time("");
        if (ret != ESP_OK) {
            s_last_error = ret;
            s_time_valid = false;
            ESP_LOGE(TAG, "periodic RTC read failed: %s", esp_err_to_name(ret));
        } else {
            s_last_error = ESP_OK;
        }
    }
}

static esp_err_t rtc_start_periodic_task(void)
{
    if (s_rtc_task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreate(rtc_periodic_task,
                                 "rtc_test_task",
                                 RX8025_TASK_STACK_SIZE,
                                 NULL,
                                 RX8025_TASK_PRIORITY,
                                 &s_rtc_task_handle);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "create RTC periodic task failed");
    ESP_LOGI(TAG, "%s periodic read task started, interval=%d ms",
             TEST_RTC_CHIP_NAME, TEST_RTC_READ_INTERVAL_MS);
    return ESP_OK;
}

esp_err_t test_rtc_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t i2c_bus_handle = NULL;
    int i2c_port = 0;
    ESP_RETURN_ON_ERROR(display_touch_get_i2c_bus(&i2c_bus_handle, &i2c_port),
                        TAG, "get shared I2C bus handle failed");

    esp_err_t ret = i2c_master_probe(i2c_bus_handle, TEST_RTC_I2C_ADDR, TEST_RTC_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s probe failed at I2C address 0x%02X: %s",
                 TEST_RTC_CHIP_NAME, TEST_RTC_I2C_ADDR, esp_err_to_name(ret));
        return ret;
    }

    const i2c_device_config_t rtc_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TEST_RTC_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus_handle, &rtc_cfg, &s_rtc_dev),
                        TAG, "add RTC I2C device failed");

    s_initialized = true;
    s_last_error = ESP_OK;
    ESP_LOGI(TAG, "%s initialized on shared I2C port %d address 0x%02X",
             TEST_RTC_CHIP_NAME, i2c_port, TEST_RTC_I2C_ADDR);
    return ESP_OK;
}

esp_err_t test_rtc_run(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "RTC not initialized, call test_rtc_init first");

    uint8_t time_regs[RX8025_TIME_REG_COUNT] = {0};
    uint8_t status_regs[RX8025_STATUS_REG_COUNT] = {0};

    esp_err_t ret = rtc_read_time_and_status(time_regs, status_regs);
    if (ret != ESP_OK) {
        s_last_error = ret;
        return ret;
    }

    rtc_time_t rtc_time = {0};
    ret = rtc_decode_time(time_regs, &rtc_time);
    if (ret == ESP_OK) {
        rtc_store_time(&rtc_time, true);
        s_synced_from_build_time = false;
        s_last_error = ESP_OK;
        rtc_log_time("", &rtc_time);
        return rtc_start_periodic_task();
    }

    ESP_LOGW(TAG, "RTC time is invalid, sync from build time");
    rtc_time_t build_time = {0};
    ESP_RETURN_ON_ERROR(rtc_get_build_time(&build_time), TAG, "get build time failed");
    rtc_log_time("build ", &build_time);
    ESP_RETURN_ON_ERROR(rtc_write_time(&build_time), TAG, "write RTC build time failed");

    ESP_RETURN_ON_ERROR(rtc_read_time_and_status(time_regs, status_regs), TAG, "read RTC after sync failed");
    ESP_RETURN_ON_ERROR(rtc_decode_time(time_regs, &rtc_time), TAG, "RTC time still invalid after sync");
    rtc_store_time(&rtc_time, true);
    s_synced_from_build_time = true;
    s_last_error = ESP_OK;
    rtc_log_time("synced ", &rtc_time);
    return rtc_start_periodic_task();
}

esp_err_t test_rtc_get_status(test_rtc_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != NULL, ESP_ERR_INVALID_ARG, TAG, "out_status is NULL");

    *out_status = (test_rtc_status_t) {
        .initialized = s_initialized,
        .time_valid = s_time_valid,
        .synced_from_build_time = s_synced_from_build_time,
        .last_error = s_last_error,
        .year = s_last_time.year,
        .month = s_last_time.month,
        .mday = s_last_time.mday,
        .hour = s_last_time.hour,
        .min = s_last_time.min,
        .sec = s_last_time.sec,
        .wday_mask = s_last_time.wday_mask,
    };
    return ESP_OK;
}

esp_err_t test_rtc_deinit(void)
{
    if (s_rtc_task_handle != NULL) {
        vTaskDelete(s_rtc_task_handle);
        s_rtc_task_handle = NULL;
    }

    if (s_rtc_dev != NULL) {
        ESP_RETURN_ON_ERROR(i2c_master_bus_rm_device(s_rtc_dev),
                            TAG, "remove RTC I2C device failed");
        s_rtc_dev = NULL;
    }

    s_initialized = false;
    s_time_valid = false;
    s_synced_from_build_time = false;
    s_last_error = ESP_OK;
    s_last_time = (rtc_time_t) {0};
    ESP_LOGI(TAG, "%s deinitialized", TEST_RTC_CHIP_NAME);
    return ESP_OK;
}
