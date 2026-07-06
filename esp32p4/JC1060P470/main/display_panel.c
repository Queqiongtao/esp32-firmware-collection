/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "display_panel.h"

#include "display_config.h"
#include "esp_check.h"
#include "esp_lcd_jd9165.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#if CONFIG_EXAMPLE_LVGL_PORT_ESP_ADAPTER
#include "esp_lv_adapter_display.h"
#endif

static const char *TAG = "display_panel";

#if CONFIG_EXAMPLE_LVGL_PORT_ESP_ADAPTER
static esp_lv_adapter_tear_avoid_mode_t display_panel_get_adapter_tear_mode(void)
{
#if CONFIG_EXAMPLE_LVGL_ADAPTER_FORCE_PARTIAL_DMA2D_TEST || CONFIG_EXAMPLE_LVGL_ADAPTER_TEAR_TRIPLE_PARTIAL
    return ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL;
#elif CONFIG_EXAMPLE_LVGL_ADAPTER_TEAR_DOUBLE_FULL
    return ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_FULL;
#else
    return ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT;
#endif
}

static uint8_t display_panel_get_frame_buffer_count(void)
{
    return esp_lv_adapter_get_required_frame_buffer_count(display_panel_get_adapter_tear_mode(),
                                                         ESP_LV_ADAPTER_ROTATE_0);
}
#else
static uint8_t display_panel_get_frame_buffer_count(void)
{
    return DISPLAY_MIPI_DSI_FRAME_BUFFER_NUM;
}
#endif

esp_err_t display_panel_init(display_panel_t *out_panel)
{
    ESP_RETURN_ON_FALSE(out_panel, ESP_ERR_INVALID_ARG, TAG, "out_panel must not be NULL");

    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
    esp_lcd_panel_handle_t panel_handle = NULL;

    const esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = DISPLAY_MIPI_DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DISPLAY_MIPI_DSI_LANE_BITRATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus), TAG, "create MIPI DSI bus failed");

    const esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io), TAG, "create MIPI DBI IO failed");

    const uint8_t num_fbs = display_panel_get_frame_buffer_count();
    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DISPLAY_MIPI_DSI_DPI_CLK_MHZ,
        .in_color_format = DISPLAY_PANEL_COLOR_FORMAT,
        .num_fbs = num_fbs,
        .video_timing = {
            .h_size = DISPLAY_PANEL_H_RES,
            .v_size = DISPLAY_PANEL_V_RES,
            .hsync_back_porch = DISPLAY_PANEL_HSYNC_BACK_PORCH,
            .hsync_pulse_width = DISPLAY_PANEL_HSYNC_PULSE_WIDTH,
            .hsync_front_porch = DISPLAY_PANEL_HSYNC_FRONT_PORCH,
            .vsync_back_porch = DISPLAY_PANEL_VSYNC_BACK_PORCH,
            .vsync_pulse_width = DISPLAY_PANEL_VSYNC_PULSE_WIDTH,
            .vsync_front_porch = DISPLAY_PANEL_VSYNC_FRONT_PORCH,
        },
    };

    jd9165_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    const esp_lcd_panel_dev_config_t lcd_dev_config = {
        .reset_gpio_num = DISPLAY_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = DISPLAY_PANEL_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };

    ESP_LOGI(TAG, "Panel=%s res=%dx%d lane_rate=%dMbps dpi_clk=%dMHz num_fbs=%d",
             DISPLAY_PANEL_NAME, DISPLAY_PANEL_H_RES, DISPLAY_PANEL_V_RES,
             DISPLAY_MIPI_DSI_LANE_BITRATE_MBPS, DISPLAY_MIPI_DSI_DPI_CLK_MHZ,
             num_fbs);

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9165(mipi_dbi_io, &lcd_dev_config, &panel_handle), TAG, "create JD9165 panel failed");

#if CONFIG_EXAMPLE_USE_DMA2D_COPY_FRAME
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_enable_dma2d(panel_handle), TAG, "enable DMA2D copy failed");
    ESP_LOGI(TAG, "DMA2D draw bitmap hook enabled");
#endif

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_handle), TAG, "reset LCD panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_handle), TAG, "init LCD panel failed");

    out_panel->panel_handle = panel_handle;
    out_panel->panel_io_handle = mipi_dbi_io;
    out_panel->h_res = DISPLAY_PANEL_H_RES;
    out_panel->v_res = DISPLAY_PANEL_V_RES;
    out_panel->num_fbs = num_fbs;
    return ESP_OK;
}
