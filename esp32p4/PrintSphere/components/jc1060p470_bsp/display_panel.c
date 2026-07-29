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
static uint8_t display_panel_get_frame_buffer_count(esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode,
                                                    esp_lv_adapter_rotation_t rotation)
{
    return esp_lv_adapter_get_required_frame_buffer_count(tear_avoid_mode, rotation);
}
#else
static uint8_t display_panel_get_frame_buffer_count(esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode,
                                                    esp_lv_adapter_rotation_t rotation)
{
    (void)tear_avoid_mode;
    (void)rotation;
    return DISPLAY_MIPI_DSI_FRAME_BUFFER_NUM;
}
#endif

esp_err_t display_panel_init(esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode,
                             esp_lv_adapter_rotation_t rotation,
                             display_panel_t *out_panel)
{
    ESP_RETURN_ON_FALSE(out_panel, ESP_ERR_INVALID_ARG, TAG, "out_panel must not be NULL");
    *out_panel = (display_panel_t) {0};

    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
    esp_lcd_panel_handle_t panel_handle = NULL;

    const esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = DISPLAY_MIPI_DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DISPLAY_MIPI_DSI_LANE_BITRATE_MBPS,
    };
    esp_err_t ret = esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "create MIPI DSI bus failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ret = esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "create MIPI DBI IO failed: %s", esp_err_to_name(ret));
        (void)esp_lcd_del_dsi_bus(mipi_dsi_bus);
        return ret;
    }

    const uint8_t num_fbs = display_panel_get_frame_buffer_count(tear_avoid_mode, rotation);
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

    ret = esp_lcd_new_panel_jd9165(mipi_dbi_io, &lcd_dev_config, &panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "create JD9165 panel failed: %s", esp_err_to_name(ret));
        (void)esp_lcd_panel_io_del(mipi_dbi_io);
        (void)esp_lcd_del_dsi_bus(mipi_dsi_bus);
        return ret;
    }

#if CONFIG_EXAMPLE_USE_DMA2D_COPY_FRAME
    ret = esp_lcd_dpi_panel_enable_dma2d(panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "enable DMA2D copy failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    ESP_LOGI(TAG, "DMA2D draw bitmap hook enabled");
#endif

    ret = esp_lcd_panel_reset(panel_handle);
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_init(panel_handle);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "initialize LCD panel failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    out_panel->panel_handle = panel_handle;
    out_panel->panel_io_handle = mipi_dbi_io;
    out_panel->dsi_bus_handle = mipi_dsi_bus;
    out_panel->h_res = DISPLAY_PANEL_H_RES;
    out_panel->v_res = DISPLAY_PANEL_V_RES;
    out_panel->num_fbs = num_fbs;
    return ESP_OK;

cleanup:
    (void)esp_lcd_panel_del(panel_handle);
    (void)esp_lcd_panel_io_del(mipi_dbi_io);
    (void)esp_lcd_del_dsi_bus(mipi_dsi_bus);
    return ret;
}

esp_err_t display_panel_deinit(display_panel_t *panel)
{
    ESP_RETURN_ON_FALSE(panel != NULL, ESP_ERR_INVALID_ARG, TAG, "panel is NULL");
    esp_err_t first_err = ESP_OK;
    if (panel->panel_handle != NULL) {
        esp_err_t ret = esp_lcd_panel_del(panel->panel_handle);
        if (first_err == ESP_OK) first_err = ret;
        if (ret == ESP_OK) panel->panel_handle = NULL;
    }
    if (panel->panel_io_handle != NULL) {
        esp_err_t ret = esp_lcd_panel_io_del(panel->panel_io_handle);
        if (first_err == ESP_OK) first_err = ret;
        if (ret == ESP_OK) panel->panel_io_handle = NULL;
    }
    if (panel->dsi_bus_handle != NULL) {
        esp_err_t ret = esp_lcd_del_dsi_bus(panel->dsi_bus_handle);
        if (first_err == ESP_OK) first_err = ret;
        if (ret == ESP_OK) panel->dsi_bus_handle = NULL;
    }
    return first_err;
}
