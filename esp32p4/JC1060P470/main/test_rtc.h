/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"

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
 * @brief Remove the RTC device handle. Safe to call when RTC is not initialized.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_rtc_deinit(void);
