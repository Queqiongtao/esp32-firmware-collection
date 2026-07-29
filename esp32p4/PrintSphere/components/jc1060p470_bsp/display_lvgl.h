/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "display_panel.h"

typedef struct {
    lv_display_t *display;
    lv_indev_t *indev;
} display_lvgl_t;

esp_err_t display_lvgl_init(const display_panel_t *panel,
                            const esp_lv_adapter_config_t *adapter_cfg,
                            esp_lv_adapter_rotation_t rotation,
                            esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode,
                            display_lvgl_t *out_lvgl);
esp_err_t display_lvgl_deinit(display_lvgl_t *lvgl);
esp_err_t display_lvgl_lock(int32_t timeout_ms);
void display_lvgl_unlock(void);
