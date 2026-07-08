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
#include "esp_timer.h"

static const char *TAG = "display_touch";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;
static portMUX_TYPE s_touch_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static display_touch_snapshot_t s_touch_snapshot;

static void display_touch_update_snapshot(const esp_lcd_touch_point_data_t *points, uint8_t count)
{
    display_touch_snapshot_t snapshot = {
        .count = count > CONFIG_ESP_LCD_TOUCH_MAX_POINTS ? CONFIG_ESP_LCD_TOUCH_MAX_POINTS : count,
        .timestamp_us = esp_timer_get_time(),
    };

    for (uint8_t i = 0; i < snapshot.count; i++) {
        snapshot.points[i].track_id = points[i].track_id;
        snapshot.points[i].x = (int32_t)((uint32_t)points[i].x * DISPLAY_PANEL_H_RES / DISPLAY_TOUCH_NATIVE_H_RES);
        snapshot.points[i].y = (int32_t)((uint32_t)points[i].y * DISPLAY_PANEL_V_RES / DISPLAY_TOUCH_NATIVE_V_RES);
        snapshot.points[i].strength = points[i].strength;
    }

    portENTER_CRITICAL(&s_touch_snapshot_lock);
    snapshot.sequence = s_touch_snapshot.sequence + 1;
    s_touch_snapshot = snapshot;
    portEXIT_CRITICAL(&s_touch_snapshot_lock);
}

esp_err_t display_touch_init(void)
{
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

        ret = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &s_touch_handle);
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

esp_err_t display_touch_get_i2c_bus(i2c_master_bus_handle_t *out_handle, int *out_port)
{
    ESP_RETURN_ON_FALSE(out_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "out_handle is NULL");
    ESP_RETURN_ON_FALSE(s_i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "I2C bus not initialized");
    *out_handle = s_i2c_bus;
    if (out_port) {
        *out_port = DISPLAY_TOUCH_I2C_NUM;
    }
    return ESP_OK;
}

esp_err_t display_touch_get_handle(esp_lcd_touch_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(out_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "out_handle is NULL");
    ESP_RETURN_ON_FALSE(s_touch_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "touch not initialized");
    *out_handle = s_touch_handle;
    return ESP_OK;
}

esp_err_t display_touch_read_cached(esp_lcd_touch_handle_t tp,
                                    esp_lcd_touch_point_data_t *points,
                                    uint8_t *count,
                                    uint8_t max_count,
                                    void *user_ctx)
{
    (void)user_ctx;

    ESP_RETURN_ON_FALSE(tp != NULL, ESP_ERR_INVALID_ARG, TAG, "touch handle is NULL");
    ESP_RETURN_ON_FALSE(points != NULL, ESP_ERR_INVALID_ARG, TAG, "points is NULL");
    ESP_RETURN_ON_FALSE(count != NULL, ESP_ERR_INVALID_ARG, TAG, "count is NULL");
    ESP_RETURN_ON_FALSE(max_count > 0, ESP_ERR_INVALID_ARG, TAG, "max_count is zero");

    *count = 0;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_read_data(tp), TAG, "read touch data failed");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_get_data(tp, points, count, max_count), TAG, "get touch data failed");
    display_touch_update_snapshot(points, *count);
    return ESP_OK;
}

esp_err_t display_touch_get_snapshot(display_touch_snapshot_t *out_snapshot)
{
    ESP_RETURN_ON_FALSE(out_snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "out_snapshot is NULL");

    portENTER_CRITICAL(&s_touch_snapshot_lock);
    *out_snapshot = s_touch_snapshot;
    portEXIT_CRITICAL(&s_touch_snapshot_lock);
    return ESP_OK;
}

static void display_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    esp_lcd_touch_point_data_t touch_data[1];
    uint8_t touch_cnt = 0;

    esp_err_t err = display_touch_read_cached(s_touch_handle, touch_data, &touch_cnt, 1, NULL);

    if (err == ESP_OK && touch_cnt > 0) {
        data->point.x = (int32_t)((uint32_t)touch_data[0].x * DISPLAY_PANEL_H_RES / DISPLAY_TOUCH_NATIVE_H_RES);
        data->point.y = (int32_t)((uint32_t)touch_data[0].y * DISPLAY_PANEL_V_RES / DISPLAY_TOUCH_NATIVE_V_RES);
        data->state = LV_INDEV_STATE_PRESSED;
        ESP_LOGD(TAG, "raw=%d,%d -> lcd=%d,%d cnt=%d",
                 touch_data[0].x, touch_data[0].y,
                 data->point.x, data->point.y, touch_cnt);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

esp_err_t display_touch_register_lvgl_indev(lv_display_t *disp, lv_indev_t **out_indev)
{
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_ERR_INVALID_ARG, TAG, "disp is NULL");
    ESP_RETURN_ON_FALSE(s_touch_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "touch not initialized");

    lv_indev_t *indev = lv_indev_create();
    ESP_RETURN_ON_FALSE(indev != NULL, ESP_ERR_NO_MEM, TAG, "create LVGL input device failed");
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, display_touch_read_cb);
    lv_indev_set_display(indev, disp);

    if (out_indev) {
        *out_indev = indev;
    }
    return ESP_OK;
}
