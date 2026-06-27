/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_mipi_dsi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_LCD_JD9165_VER_MAJOR 1
#define ESP_LCD_JD9165_VER_MINOR 0
#define ESP_LCD_JD9165_VER_PATCH 1

typedef struct {
    int cmd;
    const void *data;
    size_t data_bytes;
    unsigned int delay_ms;
} jd9165_lcd_init_cmd_t;

typedef struct {
    const jd9165_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        esp_lcd_dsi_bus_handle_t dsi_bus;
        const esp_lcd_dpi_panel_config_t *dpi_config;
    } mipi_config;
} jd9165_vendor_config_t;

esp_err_t esp_lcd_new_panel_jd9165(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel);

#define JD9165_PANEL_BUS_DSI_2CH_CONFIG()                \
    {                                                    \
        .bus_id = 0,                                     \
        .num_data_lanes = 2,                             \
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,     \
        .lane_bit_rate_mbps = 750,                       \
    }

#define JD9165_PANEL_IO_DBI_CONFIG()  \
    {                                 \
        .virtual_channel = 0,         \
        .lcd_cmd_bits = 8,            \
        .lcd_param_bits = 8,          \
    }

#define JD9165_1024_600_PANEL_60HZ_DPI_CONFIG(px_format) \
    {                                                    \
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,     \
        .dpi_clock_freq_mhz = 58,                        \
        .virtual_channel = 0,                            \
        .in_color_format = px_format,                    \
        .num_fbs = 1,                                    \
        .video_timing = {                                \
            .h_size = 1024,                              \
            .v_size = 600,                               \
            .hsync_back_porch = 160,                     \
            .hsync_pulse_width = 40,                     \
            .hsync_front_porch = 160,                    \
            .vsync_back_porch = 23,                      \
            .vsync_pulse_width = 10,                     \
            .vsync_front_porch = 12,                     \
        },                                               \
        .flags.use_dma2d = true,                         \
    }

#ifdef __cplusplus
}
#endif

#endif
