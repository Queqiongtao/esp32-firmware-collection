/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"

esp_err_t display_board_init(void);
esp_err_t display_board_set_backlight(uint32_t level);

#if CONFIG_EXAMPLE_MONITOR_REFRESH_BY_GPIO
bool display_board_monitor_refresh_rate(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx);
#endif
