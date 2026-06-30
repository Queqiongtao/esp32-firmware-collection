/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize I2C master bus and create GT911 touch handle.
 *
 * The touch handle is held internally by this module; callers do not need
 * to manage its lifetime.
 *
 * @return
 *      - ESP_OK on success
 *      - Other errors from I2C or touch driver initialization
 */
esp_err_t display_touch_init(void);

/**
 * @brief Create an LVGL pointer input device bound to the touch controller.
 *
 * Must be called after display_touch_init() and after the LVGL display has
 * been created. The created indev reads touch data and scales coordinates
 * from the touch controller's native resolution to the panel resolution.
 *
 * @param disp    LVGL display the input device is attached to.
 * @param out_indev  Optional, returns the created indev handle (may be NULL).
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if disp is NULL
 *      - ESP_ERR_NO_MEM if the indev cannot be created
 */
esp_err_t display_touch_register_lvgl_indev(lv_display_t *disp, lv_indev_t **out_indev);

#ifdef __cplusplus
}
#endif