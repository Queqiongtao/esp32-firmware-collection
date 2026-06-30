/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "display_touch.h"

#include "display_config.h"
#include "esp_check.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"

static const char *TAG = "display_touch";
static i2c_master_bus_handle_t s_i2c_bus = NULL;

esp_err_t display_touch_init(esp_lcd_touch_handle_t *ret_touch)
{
    ESP_RETURN_ON_FALSE(ret_touch != NULL, ESP_ERR_INVALID_ARG, TAG, "ret_touch is NULL");

    if (s_i2c_bus == NULL) {
        const i2c_master_bus_config_t i2c_bus_conf = {
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .sda_io_num = DISPLAY_TOUCH_I2C_SDA,
            .scl_io_num = DISPLAY_TOUCH_I2C_SCL,
            .i2c_port = DISPLAY_TOUCH_I2C_NUM,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_bus_conf, &s_i2c_bus), TAG, "create I2C master bus failed");
        ESP_LOGI(TAG, "I2C master bus initialized (SDA=%d, SCL=%d, %d kHz)",
                 DISPLAY_TOUCH_I2C_SDA, DISPLAY_TOUCH_I2C_SCL, DISPLAY_TOUCH_I2C_CLK_HZ / 1000);
    }

    /* Try primary I2C address (0x5D) first, then backup (0x14) */
    const uint8_t i2c_addrs[] = {
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP,
    };
    const char *addr_names[] = { "primary", "backup" };
    esp_err_t ret = ESP_FAIL;

    for (size_t i = 0; i < sizeof(i2c_addrs) / sizeof(i2c_addrs[0]); i++) {
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        tp_io_config.scl_speed_hz = DISPLAY_TOUCH_I2C_CLK_HZ;
        tp_io_config.dev_addr = i2c_addrs[i];

        /* driver_data tells the GT911 driver which I2C address to lock via the
         * INT-pin level during the RST release sequence. Without it the driver
         * skips the address-selection sequence and the locked address becomes
         * non-deterministic, causing intermittent init failures. */
        esp_lcd_touch_io_gt911_config_t gt911_io_config = {
            .dev_addr = i2c_addrs[i],
        };

        const esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_TOUCH_NATIVE_H_RES,
            .y_max = DISPLAY_TOUCH_NATIVE_V_RES,
            .rst_gpio_num = DISPLAY_PIN_NUM_TOUCH_RST,
            .int_gpio_num = DISPLAY_PIN_NUM_TOUCH_INT,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
            .driver_data = &gt911_io_config,
        };

        ret = esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_config, &tp_io_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "create touch panel IO failed for %s address 0x%02X", addr_names[i], i2c_addrs[i]);
            continue;
        }

        ret = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, ret_touch);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "GT911 touch initialized at %s address 0x%02X (native %dx%d, RST=%d, INT=%d)",
                     addr_names[i], i2c_addrs[i],
                     DISPLAY_TOUCH_NATIVE_H_RES, DISPLAY_TOUCH_NATIVE_V_RES,
                     DISPLAY_PIN_NUM_TOUCH_RST, DISPLAY_PIN_NUM_TOUCH_INT);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "GT911 init failed at %s address 0x%02X: %s",
                 addr_names[i], i2c_addrs[i], esp_err_to_name(ret));
        esp_lcd_panel_io_del(tp_io_handle);
        tp_io_handle = NULL;
    }

    ESP_LOGE(TAG, "create GT911 touch controller failed on all addresses");
    return ret;
}
