/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "display_panel.h"

typedef struct {
    lv_display_t *display;
    lv_indev_t *indev;
} display_lvgl_t;

esp_err_t display_lvgl_init(const display_panel_t *panel, display_lvgl_t *out_lvgl);
void display_lvgl_lock(void);
void display_lvgl_unlock(void);
