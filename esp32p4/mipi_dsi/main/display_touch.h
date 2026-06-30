/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize I2C master bus and create GT911 touch handle.
 *
 * @param[out] ret_touch  Returned GT911 touch handle.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if ret_touch is NULL
 *      - Other errors from I2C or touch driver initialization
 */
esp_err_t display_touch_init(esp_lcd_touch_handle_t *ret_touch);

#ifdef __cplusplus
}
#endif
