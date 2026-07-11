#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "frame_pipeline.h"
#include "subscreen_display.h"
#include "transport_usb.h"
#include "transport_wifi.h"

static const char *TAG = "companion_display";

extern "C" void app_main(void)
{
    esp_chip_info_t chip{};
    uint32_t flash_size = 0;
    esp_chip_info(&chip);
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "Companion Display starting: cores=%d flash=%luMB", chip.cores,
             (unsigned long)(flash_size / (1024 * 1024)));

    subscreen_display_t *display = nullptr;
    ESP_ERROR_CHECK(subscreen_display_init(&display));
    ESP_ERROR_CHECK(subscreen_display_show_placeholder(display));
    ESP_ERROR_CHECK(frame_pipeline_init(display));
    ESP_ERROR_CHECK(transport_usb_init());
    ESP_ERROR_CHECK(transport_wifi_init(display));
}
