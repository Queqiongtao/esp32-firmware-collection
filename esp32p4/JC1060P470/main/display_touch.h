/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GT911 touch controller native coordinate range. */
#define DISPLAY_TOUCH_NATIVE_H_RES  800
#define DISPLAY_TOUCH_NATIVE_V_RES  480

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
esp_err_t display_touch_get_handle(esp_lcd_touch_handle_t *out_handle);
esp_err_t display_touch_read_cached(esp_lcd_touch_handle_t tp,
                                    esp_lcd_touch_point_data_t *points,
                                    uint8_t *count,
                                    uint8_t max_count,
                                    void *user_ctx);
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
 * @brief Get the I2C master bus handle and port used by the touch module.
 *        Allows other modules (e.g. audio codec) to reuse the same I2C bus.
 *
 * @param out_handle  Returns the I2C master bus handle (must not be NULL).
 * @param out_port    Returns the I2C port number (may be NULL).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out_handle is NULL,
 *         ESP_ERR_INVALID_STATE if touch not initialized.
 */
esp_err_t display_touch_get_i2c_bus(i2c_master_bus_handle_t *out_handle, int *out_port);

#ifdef __cplusplus
}
#endif
