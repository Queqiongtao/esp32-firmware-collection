#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QMI8658_I2C_ADDRESS_LOW 0x6A
#define QMI8658_I2C_ADDRESS_HIGH 0x6B
#define QMI8658_DEFAULT_TIMEOUT_MS 1000

typedef struct {
    i2c_master_bus_handle_t bus_handle;
    uint16_t device_address;
    uint32_t scl_speed_hz;
    int timeout_ms;
    bool enable_gyro;
} qmi8658_config_t;

typedef struct {
    i2c_master_dev_handle_t dev_handle;
    uint16_t device_address;
    uint8_t chip_id;
    int timeout_ms;
    float accel_lsb_per_g;
    float gyro_lsb_per_dps;
} qmi8658_t;

typedef struct {
    float accel_g[3];
    float gyro_dps[3];
    float temperature_c;
    int16_t raw_accel[3];
    int16_t raw_gyro[3];
    int16_t raw_temperature;
} qmi8658_sample_t;

typedef struct {
    float accel_g[3];
    int16_t raw_accel[3];
} qmi8658_accel_sample_t;

esp_err_t qmi8658_init(qmi8658_t *imu, const qmi8658_config_t *config);
esp_err_t qmi8658_read_accel(qmi8658_t *imu, qmi8658_accel_sample_t *sample);
esp_err_t qmi8658_read(qmi8658_t *imu, qmi8658_sample_t *sample);

#ifdef __cplusplus
}
#endif
