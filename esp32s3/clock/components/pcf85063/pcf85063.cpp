#include "pcf85063.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "pcf85063";

static constexpr uint8_t REG_CONTROL_1 = 0x00;
static constexpr uint8_t REG_SECONDS = 0x04;
static constexpr uint8_t CONTROL_1_STOP = BIT(5);
static constexpr uint8_t SECONDS_OS = BIT(7);

static uint8_t bcd_to_dec(uint8_t value)
{
    return ((value >> 4) * 10) + (value & 0x0f);
}

static uint8_t dec_to_bcd(uint8_t value)
{
    return ((value / 10) << 4) | (value % 10);
}

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

static bool is_time_in_chip_range(const struct tm *timeinfo)
{
    const int year = timeinfo->tm_year + 1900;
    const int month = timeinfo->tm_mon + 1;
    const int day = timeinfo->tm_mday;

    if (year < 2000 || year > 2099) {
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

static esp_err_t read_reg(pcf85063_t *rtc, uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(
        rtc->dev_handle, &reg, sizeof(reg), value, sizeof(*value), rtc->timeout_ms);
}

static esp_err_t write_reg(pcf85063_t *rtc, uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(rtc->dev_handle, data, sizeof(data), rtc->timeout_ms);
}

static esp_err_t clear_stop(pcf85063_t *rtc)
{
    uint8_t control = 0;
    ESP_RETURN_ON_ERROR(read_reg(rtc, REG_CONTROL_1, &control), TAG, "read control failed");

    if ((control & CONTROL_1_STOP) == 0) {
        return ESP_OK;
    }

    return write_reg(rtc, REG_CONTROL_1, control & ~CONTROL_1_STOP);
}

esp_err_t pcf85063_init(pcf85063_t *rtc, const pcf85063_config_t *config)
{
    ESP_RETURN_ON_FALSE(rtc != NULL, ESP_ERR_INVALID_ARG, TAG, "rtc is null");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is null");
    ESP_RETURN_ON_FALSE(config->bus_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "bus handle is null");
    ESP_RETURN_ON_FALSE(config->scl_speed_hz > 0, ESP_ERR_INVALID_ARG, TAG, "invalid scl speed");

    memset(rtc, 0, sizeof(*rtc));
    rtc->timeout_ms = config->timeout_ms > 0 ? config->timeout_ms : PCF85063_DEFAULT_TIMEOUT_MS;

    i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = config->device_address != 0 ?
                                config->device_address : PCF85063_DEFAULT_I2C_ADDRESS;
    dev_config.scl_speed_hz = config->scl_speed_hz;

    return i2c_master_bus_add_device(config->bus_handle, &dev_config, &rtc->dev_handle);
}

esp_err_t pcf85063_get_time(pcf85063_t *rtc, struct tm *timeinfo)
{
    ESP_RETURN_ON_FALSE(rtc != NULL && rtc->dev_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "rtc is invalid");
    ESP_RETURN_ON_FALSE(timeinfo != NULL, ESP_ERR_INVALID_ARG, TAG, "timeinfo is null");

    uint8_t control = 0;
    ESP_RETURN_ON_ERROR(read_reg(rtc, REG_CONTROL_1, &control), TAG, "read control failed");
    if ((control & CONTROL_1_STOP) != 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t reg = REG_SECONDS;
    uint8_t data[7] = {};
    ESP_RETURN_ON_ERROR(
        i2c_master_transmit_receive(
            rtc->dev_handle, &reg, sizeof(reg), data, sizeof(data), rtc->timeout_ms),
        TAG, "read time failed");

    if ((data[0] & SECONDS_OS) != 0) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(timeinfo, 0, sizeof(*timeinfo));
    timeinfo->tm_sec = bcd_to_dec(data[0] & 0x7f);
    timeinfo->tm_min = bcd_to_dec(data[1] & 0x7f);
    timeinfo->tm_hour = bcd_to_dec(data[2] & 0x3f);
    timeinfo->tm_mday = bcd_to_dec(data[3] & 0x3f);
    timeinfo->tm_wday = bcd_to_dec(data[4] & 0x07);
    timeinfo->tm_mon = bcd_to_dec(data[5] & 0x1f) - 1;
    timeinfo->tm_year = bcd_to_dec(data[6]) + 100;
    timeinfo->tm_isdst = -1;

    if (!is_time_in_chip_range(timeinfo)) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t pcf85063_set_time(pcf85063_t *rtc, const struct tm *timeinfo)
{
    ESP_RETURN_ON_FALSE(rtc != NULL && rtc->dev_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "rtc is invalid");
    ESP_RETURN_ON_FALSE(timeinfo != NULL, ESP_ERR_INVALID_ARG, TAG, "timeinfo is null");
    ESP_RETURN_ON_FALSE(is_time_in_chip_range(timeinfo), ESP_ERR_INVALID_ARG, TAG, "time out of range");

    const int year = timeinfo->tm_year + 1900;
    const uint8_t data[] = {
        REG_SECONDS,
        dec_to_bcd(timeinfo->tm_sec),
        dec_to_bcd(timeinfo->tm_min),
        dec_to_bcd(timeinfo->tm_hour),
        dec_to_bcd(timeinfo->tm_mday),
        dec_to_bcd(timeinfo->tm_wday),
        dec_to_bcd(timeinfo->tm_mon + 1),
        dec_to_bcd(year % 100),
    };

    ESP_RETURN_ON_ERROR(
        i2c_master_transmit(rtc->dev_handle, data, sizeof(data), rtc->timeout_ms),
        TAG, "write time failed");

    return clear_stop(rtc);
}
