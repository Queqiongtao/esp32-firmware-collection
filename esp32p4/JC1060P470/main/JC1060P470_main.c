/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "display_board.h"
#include "display_lvgl.h"
#include "display_panel.h"
#include "esp_log.h"
#include "test_sdcard.h"
#include "test_rtc.h"
#include "test_audio.h"
#include "test_usb.h"

extern void example_lvgl_demo_ui(lv_display_t *disp);

static const char *TAG = "app_main";

void app_main(void)
{
    display_panel_t panel = {0};
    display_lvgl_t lvgl_port = {0};

    ESP_ERROR_CHECK(display_board_init());
    ESP_ERROR_CHECK(display_panel_init(&panel));
    ESP_ERROR_CHECK(display_board_set_backlight(100));
    ESP_ERROR_CHECK(display_lvgl_init(&panel, &lvgl_port));

    ESP_LOGI(TAG, "Display LVGL demo UI");
    display_lvgl_lock();
    example_lvgl_demo_ui(lvgl_port.display);
    display_lvgl_unlock();

#if CONFIG_EXAMPLE_ENABLE_TEST_SDCARD
    ESP_LOGI(TAG, "Initialize SD card test module");
    esp_err_t sd_ret = test_sdcard_init();
    if (sd_ret == ESP_OK) {
        test_sdcard_run();
    } else {
        ESP_LOGE(TAG, "SD card test module init failed: %s", esp_err_to_name(sd_ret));
    }
#endif

#if CONFIG_EXAMPLE_ENABLE_TEST_RTC
    ESP_LOGI(TAG, "Initialize RX8025T-UC RTC test module");
    esp_err_t rtc_ret = test_rtc_init();
    if (rtc_ret == ESP_OK) {
        test_rtc_run();
    } else {
        ESP_LOGE(TAG, "RTC test module init failed: %s", esp_err_to_name(rtc_ret));
    }
#endif

#if CONFIG_EXAMPLE_ENABLE_TEST_AUDIO
    ESP_LOGI(TAG, "Initialize audio test module");
    esp_err_t audio_ret = test_audio_init();
    if (audio_ret == ESP_OK) {
        test_audio_run();
    } else {
        ESP_LOGE(TAG, "Audio test module init failed: %s", esp_err_to_name(audio_ret));
    }
#endif

#if CONFIG_EXAMPLE_ENABLE_TEST_USB
    ESP_LOGI(TAG, "Initialize USB Device CDC-ACM test module");
    esp_err_t usb_ret = test_usb_init();
    if (usb_ret == ESP_OK) {
        test_usb_run();
    } else {
        ESP_LOGE(TAG, "USB test module init failed: %s", esp_err_to_name(usb_ret));
    }
#endif
}
