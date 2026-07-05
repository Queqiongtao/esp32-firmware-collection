/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool initialized;
    bool calibration_enabled;
    bool sample_valid;
    int raw_average;
    int adc_voltage_mv;
    int battery_voltage_mv;
    int battery_percent;
    uint32_t sample_count;
    int64_t last_update_ms;
    esp_err_t last_error;
} test_battery_status_t;

/**
 * @brief Initialize the battery voltage sampling module on GPIO52.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_battery_init(void);

/**
 * @brief Refresh one battery sample if the configured update period has elapsed.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_battery_run(void);

/**
 * @brief Get a read-only snapshot of the battery module state.
 *
 * @return Pointer to the module status structure.
 */
const test_battery_status_t *test_battery_get_status(void);

/**
 * @brief Release the ADC oneshot and calibration resources.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_battery_deinit(void);
