#include "companion_board.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "frame_pipeline.h"
#include "subscreen_display.h"
#include "transport_wifi.h"

static const char *TAG = "companion_display";

#define NETWORK_RETRY_DELAY_MS 2000

extern "C" void app_main(void)
{
    esp_chip_info_t chip{};
    uint32_t flash_size = 0;
    esp_chip_info(&chip);
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "Companion Display P4 starting: cores=%d flash=%luMB", chip.cores,
             (unsigned long)(flash_size / (1024 * 1024)));

    subscreen_display_t *display = nullptr;
    ESP_ERROR_CHECK(subscreen_display_init(&display));
    ESP_ERROR_CHECK(subscreen_display_show_placeholder(display));
    ESP_ERROR_CHECK(frame_pipeline_init(display));
    ESP_ERROR_CHECK(subscreen_display_set_network_status(
        display, SUBSCREEN_NETWORK_CONNECTING, "", ""));

    /* Keep the display responsive while the C6 is absent or still booting. */
    esp_err_t network_err;
    while ((network_err = board_network_init()) != ESP_OK) {
        ESP_LOGW(TAG, "ESP-Hosted unavailable (%s), retrying in %d ms",
                 esp_err_to_name(network_err), NETWORK_RETRY_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(NETWORK_RETRY_DELAY_MS));
    }
    ESP_ERROR_CHECK(transport_wifi_init(display));
}
