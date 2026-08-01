#include <inttypes.h>
#include <stdio.h>

#include "board_amoled_1_8.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "fluid_render.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "amoled_fluid";
static constexpr int TOUCH_POLL_INTERVAL_MS = 10;

extern "C" void app_main(void)
{
    esp_chip_info_t chip_info{};
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "Waveshare ESP32-S3 Touch AMOLED 1.8 project starting");
    ESP_LOGI(TAG,
             "cores=%d, flash=%" PRIu32 "MB, psram=%s",
             chip_info.cores,
             flash_size / (1024 * 1024),
             (chip_info.features & CHIP_FEATURE_EMB_PSRAM) ? "embedded" : "external or unknown");

    const board_amoled_1_8_config_t *cfg = board_amoled_1_8_get_config();
    ESP_LOGI(TAG, "board cfg: lcd(cs=%d pclk=%d d0-d3=%d/%d/%d/%d te=%d exio_pwr=%d exio_rst=%d %dx%d)",
             cfg->lcd_cs_gpio, cfg->lcd_pclk_gpio,
             cfg->lcd_d0_gpio, cfg->lcd_d1_gpio, cfg->lcd_d2_gpio, cfg->lcd_d3_gpio,
             cfg->lcd_te_gpio, cfg->lcd_power_exio_num, cfg->lcd_rst_exio_num,
             cfg->lcd_h_res, cfg->lcd_v_res);
    ESP_LOGI(TAG, "board cfg: touch(int=%d gpio_rst=%d exio_rst=%d)",
             cfg->touch_int_gpio, cfg->touch_rst_gpio, cfg->touch_rst_exio_num);

    esp_err_t err = board_display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board init failed: %s", esp_err_to_name(err));
    }

    esp_lcd_panel_handle_t panel = NULL;
    err = board_display_new(&panel, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board display new failed: %s", esp_err_to_name(err));
    }

    if (cfg->display_delay_i2c_scan) {
        err = board_i2c_scan();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2c scan failed: %s", esp_err_to_name(err));
        }
    }

    fluid_render_handle_t fluid = NULL;
    if (panel != NULL) {
        fluid_render_config_t fluid_cfg = fluid_render_default_config();
        fluid_cfg.h_res = cfg->lcd_h_res;
        fluid_cfg.v_res = cfg->lcd_v_res;
        err = fluid_render_create(&fluid_cfg, panel, &fluid);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "fluid render create failed: %s", esp_err_to_name(err));
        } else {
            err = fluid_render_start(fluid);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "fluid render start failed: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "fluid render started (%dx%d grid @ %d Hz, %d blobs)",
                         fluid_cfg.sim_w, fluid_cfg.sim_h, fluid_cfg.sim_hz, fluid_cfg.blob_count);
            }
        }
    } else {
        ESP_LOGE(TAG, "display unavailable, fluid effect disabled");
    }

    esp_lcd_touch_handle_t touch = NULL;
    err = board_touch_new(&touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board touch new failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "touch self-test armed");
    }

    bool touch_active = false;
    uint16_t last_touch_x = 0;
    uint16_t last_touch_y = 0;

    while (1) {
        if (touch != NULL) {
            uint16_t touch_x = 0;
            uint16_t touch_y = 0;
            uint8_t pressed = 0;
            uint8_t point_num = 0;
            err = board_touch_read(&touch_x, &touch_y, &pressed, &point_num);
            const bool release_event = (!pressed && point_num == BOARD_TOUCH_RELEASE_EVENT_POINT_NUM);
            if (err != ESP_OK) {
                if (touch_active) {
                    fluid_render_touch_event(fluid, false, last_touch_x, last_touch_y);
                }
                ESP_LOGW(TAG, "touch read failed: %s", esp_err_to_name(err));
                touch_active = false;
            } else if (pressed) {
                if (!touch_active) {
                    ESP_LOGI(TAG, "touch down: x=%u y=%u points=%u", touch_x, touch_y, point_num);
                }
                fluid_render_touch_event(fluid, true, touch_x, touch_y);
                touch_active = true;
                last_touch_x = touch_x;
                last_touch_y = touch_y;
            } else if (touch_active || release_event) {
                const uint16_t release_x = release_event ? touch_x : last_touch_x;
                const uint16_t release_y = release_event ? touch_y : last_touch_y;
                ESP_LOGI(TAG, "touch up: x=%u y=%u", release_x, release_y);
                fluid_render_touch_event(fluid, false, release_x, release_y);
                touch_active = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_INTERVAL_MS));
    }
}
