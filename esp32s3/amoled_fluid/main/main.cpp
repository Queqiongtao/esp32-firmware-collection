#include <inttypes.h>
#include <stdio.h>

#include "board_amoled_1_8.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "amoled_fluid";
static constexpr int TOUCH_POLL_INTERVAL_MS = 10;
static constexpr int TOUCH_MOVE_THRESHOLD_PX = 2;

static esp_err_t draw_solid_color(esp_lcd_panel_handle_t panel, int h_res, int v_res, uint16_t color, const char *name)
{
    const int chunk_lines = 32;
    uint16_t *line_buf = (uint16_t *)heap_caps_malloc((size_t)h_res * chunk_lines * sizeof(uint16_t),
                                                      MALLOC_CAP_SPIRAM);
    if (line_buf == NULL) {
        ESP_LOGE(TAG, "solid color buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < h_res * chunk_lines; ++i) {
        line_buf[i] = color;
    }

    for (int y = 0; y < v_res; y += chunk_lines) {
        const int y2 = (y + chunk_lines < v_res) ? (y + chunk_lines) : v_res;
        esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, y, h_res, y2, line_buf);
        if (err != ESP_OK) {
            free(line_buf);
            ESP_LOGE(TAG, "draw %s failed at lines [%d,%d): %s", name, y, y2, esp_err_to_name(err));
            return err;
        }
    }

    free(line_buf);
    ESP_LOGI(TAG, "draw solid %s done", name);
    return ESP_OK;
}

static esp_err_t draw_colorbar(esp_lcd_panel_handle_t panel, int h_res, int v_res)
{
    static const uint16_t colors[] = {
        0xF800, 0x07E0, 0x001F, 0x07FF,
        0xF81F, 0xFFE0, 0xFFFF, 0x0000,
    };
    const int n = (int)(sizeof(colors) / sizeof(colors[0]));
    const int stripe_w = h_res / n;

    uint16_t *fb = (uint16_t *)heap_caps_malloc((size_t)h_res * v_res * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (fb == NULL) {
        ESP_LOGE(TAG, "framebuffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    for (int y = 0; y < v_res; y++) {
        for (int x = 0; x < h_res; x++) {
            int c = x / stripe_w;
            if (c >= n) c = n - 1;
            fb[y * h_res + x] = colors[c];
        }
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, 0, h_res, v_res, fb);
    free(fb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "colorbar draw failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "colorbar drawn %dx%d", h_res, v_res);
    return ESP_OK;
}

static void run_display_self_test(esp_lcd_panel_handle_t panel, int h_res, int v_res)
{
    if (panel == NULL) {
        ESP_LOGE(TAG, "display self-test skipped: panel is null");
        return;
    }

    ESP_LOGI(TAG, "display self-test start");
    esp_err_t err = esp_lcd_panel_disp_on_off(panel, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display on command failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "display on command sent");
    }
    vTaskDelay(pdMS_TO_TICKS(120));

    const struct {
        uint16_t color;
        const char *name;
    } frames[] = {
        {0xFFFF, "white"},
        {0xF800, "red"},
        {0x07E0, "green"},
        {0x001F, "blue"},
    };

    for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); ++i) {
        if (draw_solid_color(panel, h_res, v_res, frames[i].color, frames[i].name) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(220));
        }
    }

    draw_colorbar(panel, h_res, v_res);
    ESP_LOGI(TAG, "display self-test end");
}

static bool touch_moved_significantly(uint16_t prev_x, uint16_t prev_y, uint16_t x, uint16_t y)
{
    const int dx = (int)x - (int)prev_x;
    const int dy = (int)y - (int)prev_y;
    return (dx >= TOUCH_MOVE_THRESHOLD_PX || dx <= -TOUCH_MOVE_THRESHOLD_PX ||
            dy >= TOUCH_MOVE_THRESHOLD_PX || dy <= -TOUCH_MOVE_THRESHOLD_PX);
}

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
    if (panel) {
        if (cfg->display_self_test_delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(cfg->display_self_test_delay_ms));
        }
        run_display_self_test(panel, cfg->lcd_h_res, cfg->lcd_v_res);
    }

    if (cfg->display_delay_i2c_scan) {
        err = board_i2c_scan();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2c scan failed: %s", esp_err_to_name(err));
        }
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
                    ESP_LOGI(TAG, "touch up(interrupted): last_x=%u last_y=%u", last_touch_x, last_touch_y);
                }
                ESP_LOGW(TAG, "touch read failed: %s", esp_err_to_name(err));
                touch_active = false;
            } else if (pressed) {
                if (!touch_active) {
                    ESP_LOGI(TAG, "touch down: x=%u y=%u points=%u", touch_x, touch_y, point_num);
                } else if (touch_moved_significantly(last_touch_x, last_touch_y, touch_x, touch_y)) {
                    ESP_LOGI(TAG, "touch move: x=%u y=%u points=%u", touch_x, touch_y, point_num);
                }
                touch_active = true;
                last_touch_x = touch_x;
                last_touch_y = touch_y;
            } else if (touch_active || release_event) {
                const uint16_t release_x = release_event ? touch_x : last_touch_x;
                const uint16_t release_y = release_event ? touch_y : last_touch_y;
                if (!touch_active && release_event) {
                    ESP_LOGI(TAG, "touch up(resync): last_x=%u last_y=%u", release_x, release_y);
                } else {
                    ESP_LOGI(TAG, "touch up: last_x=%u last_y=%u", release_x, release_y);
                }
                touch_active = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_INTERVAL_MS));
    }
}
