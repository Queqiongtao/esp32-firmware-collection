#include <inttypes.h>
#include <stdio.h>
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "subscreen_display.h"
#include "usb_extend_screen.h"

static const char *TAG = "amoled_subscreen";

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

    subscreen_display_t *display = NULL;
    esp_err_t err = subscreen_display_init(&display);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(subscreen_display_show_placeholder(display));
    err = usb_extend_screen_init(display);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB extend screen init failed: %s", esp_err_to_name(err));
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
