#pragma once

#include <time.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCF85063_DEFAULT_I2C_ADDRESS 0x51
#define PCF85063_DEFAULT_TIMEOUT_MS 1000

typedef struct {
    i2c_master_bus_handle_t bus_handle;
    uint16_t device_address;
    uint32_t scl_speed_hz;
    int timeout_ms;
} pcf85063_config_t;

typedef struct {
    i2c_master_dev_handle_t dev_handle;
    int timeout_ms;
} pcf85063_t;

esp_err_t pcf85063_init(pcf85063_t *rtc, const pcf85063_config_t *config);
esp_err_t pcf85063_get_time(pcf85063_t *rtc, struct tm *timeinfo);
esp_err_t pcf85063_set_time(pcf85063_t *rtc, const struct tm *timeinfo);

#ifdef __cplusplus
}
#endif
