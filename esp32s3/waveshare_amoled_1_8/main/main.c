#include <inttypes.h>
#include <stdio.h>

#include "board_amoled_1_8.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "waveshare_amoled_1_8";

void app_main(void)
{
    esp_chip_info_t chip_info = {0};
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "Waveshare ESP32-S3 Touch AMOLED 1.8 project starting");
    ESP_LOGI(TAG,
             "cores=%d, flash=%" PRIu32 "MB, psram=%s",
             chip_info.cores,
             flash_size / (1024 * 1024),
             (chip_info.features & CHIP_FEATURE_EMB_PSRAM) ? "embedded" : "external or unknown");

    const board_amoled_1_8_config_t *board_cfg = board_amoled_1_8_get_config();
    ESP_LOGI(TAG,
             "board cfg: i2c(port=%d sda=%d scl=%d) backlight_gpio=%d",
             board_cfg->i2c_port,
             board_cfg->sda_gpio,
             board_cfg->scl_gpio,
             board_cfg->backlight_gpio);

    esp_err_t err = board_display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board init failed: %s", esp_err_to_name(err));
    }

    while (1) {
        ESP_LOGI(TAG, "project skeleton is running");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
