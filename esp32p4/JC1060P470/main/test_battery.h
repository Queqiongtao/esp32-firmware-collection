/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "test_common.h"

typedef struct {
    test_module_meta_t module;
    bool initialized;
    bool running;
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
 * @brief Start the low-priority periodic battery sampling worker.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_battery_run(void);

/**
 * @brief Copy a consistent snapshot of the battery module state.
 *
 * @param out_status  Output status buffer.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out_status is NULL.
 */
esp_err_t test_battery_get_status(test_battery_status_t *out_status);

/**
 * @brief Release the ADC oneshot and calibration resources.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_battery_deinit(void);
