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
    bool time_valid;
    bool synced_from_build_time;
    esp_err_t last_error;
    uint16_t year;
    uint8_t month;
    uint8_t mday;
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
    uint8_t wday_mask;
} test_rtc_status_t;

/**
 * @brief Attach the RX8025T-UC RTC to the shared touch I2C bus and probe it.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_rtc_init(void);

/**
 * @brief Read RTC time/status registers and print decoded time to the log.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_rtc_run(void);

/**
 * @brief Get a snapshot of the RTC module state.
 *
 * @param out_status  Output status buffer.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out_status is NULL.
 */
esp_err_t test_rtc_get_status(test_rtc_status_t *out_status);

/**
 * @brief Remove the RTC device handle. Safe to call when RTC is not initialized.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_rtc_deinit(void);
