/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdint.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t track_id;
    int32_t x;
    int32_t y;
    uint16_t strength;
} display_touch_point_t;

typedef struct {
    uint8_t count;
    display_touch_point_t points[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    int64_t timestamp_us;
    uint32_t sequence;
} display_touch_snapshot_t;

/**
 * @brief Create the GT911 device on the board-owned shared I2C bus.
 *
 * The touch handle is held internally by this module; callers do not need
 * to manage its lifetime.
 *
 * @return
 *      - ESP_OK on success
 *      - Other errors from I2C or touch driver initialization
 */
esp_err_t display_touch_init(void);
esp_err_t display_touch_deinit(void);
esp_err_t display_touch_get_snapshot(display_touch_snapshot_t *out_snapshot);

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

/**
 * @brief Unregister the LVGL input device before releasing touch hardware.
 *
 * The legacy LVGL port caller must hold the LVGL API lock while calling this
 * function. The esp_lvgl_adapter path manages its lock internally.
 */
esp_err_t display_touch_unregister_lvgl_indev(void);

#ifdef __cplusplus
}
#endif
