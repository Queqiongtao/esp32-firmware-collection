/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lv_adapter.h"

typedef struct {
    esp_lcd_panel_handle_t panel_handle;
    esp_lcd_panel_io_handle_t panel_io_handle;
    esp_lcd_dsi_bus_handle_t dsi_bus_handle;
    uint16_t h_res;
    uint16_t v_res;
    uint8_t num_fbs;
} display_panel_t;

esp_err_t display_panel_init(esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode,
                             esp_lv_adapter_rotation_t rotation,
                             display_panel_t *out_panel);
esp_err_t display_panel_deinit(display_panel_t *panel);
