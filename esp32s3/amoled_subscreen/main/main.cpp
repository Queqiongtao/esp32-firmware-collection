#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "board_amoled_1_8.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "amoled_subscreen";

static constexpr int PLACEHOLDER_CHUNK_LINES = 32;
static constexpr uint16_t COLOR_BG = 0x1084;
static constexpr uint16_t COLOR_HEADER = 0x07FF;
static constexpr uint16_t COLOR_ACCENT = 0xFBE0;
static constexpr uint16_t COLOR_TRACK = 0x39E7;
static constexpr uint16_t COLOR_CARD = 0x2945;

static uint16_t placeholder_pixel(int x, int y, int h_res, int v_res)
{
    if (y < 42) {
        return COLOR_HEADER;
    }
    if (y >= v_res - 26) {
        return COLOR_ACCENT;
    }
    if (x < 10 || x >= h_res - 10 || y < 52 || y >= v_res - 36) {
        return COLOR_TRACK;
    }
    if ((x >= 34 && x < h_res - 34 && y >= 92 && y < 174) ||
        (x >= 34 && x < h_res - 34 && y >= 208 && y < 290) ||
        (x >= 34 && x < h_res - 34 && y >= 324 && y < 388)) {
        return COLOR_CARD;
    }
    if ((x / 18 + y / 18) % 9 == 0) {
        return 0x2104;
    }
    return COLOR_BG;
}

static esp_err_t draw_placeholder(esp_lcd_panel_handle_t panel, int h_res, int v_res)
{
    uint16_t *line_buf = (uint16_t *)heap_caps_malloc((size_t)h_res * PLACEHOLDER_CHUNK_LINES * sizeof(uint16_t),
                                                      MALLOC_CAP_SPIRAM);
    if (line_buf == NULL) {
        ESP_LOGE(TAG, "placeholder buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    for (int y = 0; y < v_res; y += PLACEHOLDER_CHUNK_LINES) {
        const int y2 = (y + PLACEHOLDER_CHUNK_LINES < v_res) ? (y + PLACEHOLDER_CHUNK_LINES) : v_res;
        for (int row = y; row < y2; ++row) {
            uint16_t *dst = line_buf + (row - y) * h_res;
            for (int x = 0; x < h_res; ++x) {
                dst[x] = placeholder_pixel(x, row, h_res, v_res);
            }
        }

        esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, y, h_res, y2, line_buf);
        if (err != ESP_OK) {
            free(line_buf);
            ESP_LOGE(TAG, "draw placeholder failed at lines [%d,%d): %s", y, y2, esp_err_to_name(err));
            return err;
        }
    }

    free(line_buf);
    ESP_LOGI(TAG, "placeholder frame drawn %dx%d", h_res, v_res);
    return ESP_OK;
}

extern "C" void app_main(void)
{
    esp_chip_info_t chip_info{};
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "AMOLED subscreen project starting");
    ESP_LOGI(TAG,
             "cores=%d, flash=%" PRIu32 "MB, psram=%s",
             chip_info.cores,
             flash_size / (1024 * 1024),
             (chip_info.features & CHIP_FEATURE_EMB_PSRAM) ? "embedded" : "external or unknown");

    const board_amoled_1_8_config_t *cfg = board_amoled_1_8_get_config();
    ESP_LOGI(TAG, "display cfg: cs=%d pclk=%d d0-d3=%d/%d/%d/%d %dx%d",
             cfg->lcd_cs_gpio,
             cfg->lcd_pclk_gpio,
             cfg->lcd_d0_gpio,
             cfg->lcd_d1_gpio,
             cfg->lcd_d2_gpio,
             cfg->lcd_d3_gpio,
             cfg->lcd_h_res,
             cfg->lcd_v_res);

    esp_err_t err = board_display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
    }

    esp_lcd_panel_handle_t panel = NULL;
    err = board_display_new(&panel, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display panel get failed: %s", esp_err_to_name(err));
    } else if (panel != NULL) {
        err = draw_placeholder(panel, cfg->lcd_h_res, cfg->lcd_v_res);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "placeholder draw failed: %s", esp_err_to_name(err));
        }
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
