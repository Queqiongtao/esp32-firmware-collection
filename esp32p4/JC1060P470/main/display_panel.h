/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

typedef struct {
    esp_lcd_panel_handle_t panel_handle;
    uint16_t h_res;
    uint16_t v_res;
} display_panel_t;

esp_err_t display_panel_init(display_panel_t *out_panel);
