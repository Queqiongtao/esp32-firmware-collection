/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "display_board.h"
#include "display_font.h"
#include "display_lvgl.h"
#include "display_panel.h"
#include "esp_cpu.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "test_sdcard.h"
#include "test_rtc.h"
#include "test_audio.h"
#include "test_battery.h"
#include "test_usb.h"
#include "test_wireless.h"

extern void example_lvgl_demo_ui(lv_display_t *disp);

static const char *TAG = "app_main";
static StaticTask_t DRAM_ATTR s_system_init_task_tcb;
static StackType_t DRAM_ATTR s_system_init_task_stack[4096];

static esp_err_t app_prepare_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
        ret = nvs_flash_init();
    }
    return ret;
}

static void app_system_init_task(void *arg)
{
    (void)arg;

    void *sp = esp_cpu_get_sp();
    ESP_LOGI(TAG, "System init task start: core=%d sp=%p stack_buf=%p sp_in_dram=%d stack_in_dram=%d",
             (int)xPortGetCoreID(),
             sp,
             s_system_init_task_stack,
             esp_ptr_in_dram(sp),
             esp_ptr_in_dram(s_system_init_task_stack));

    ESP_LOGI(TAG, "Initialize system NVS");
    esp_err_t nvs_ret = app_prepare_nvs();
    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "System NVS init failed: %s", esp_err_to_name(nvs_ret));
    }

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

#if CONFIG_EXAMPLE_ENABLE_TEST_BATTERY
    ESP_LOGI(TAG, "Initialize battery monitor test module");
    esp_err_t battery_ret = test_battery_init();
    if (battery_ret == ESP_OK) {
        test_battery_run();
    } else {
        ESP_LOGE(TAG, "Battery test module init failed: %s", esp_err_to_name(battery_ret));
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

#if CONFIG_EXAMPLE_ENABLE_TEST_WIRELESS
    if (nvs_ret == ESP_OK) {
        ESP_LOGI(TAG, "Initialize wireless test module");
        esp_err_t wireless_ret = test_wireless_init();
        if (wireless_ret == ESP_OK) {
            test_wireless_run();
        } else {
            ESP_LOGE(TAG, "Wireless test module init failed: %s", esp_err_to_name(wireless_ret));
        }
    } else {
        ESP_LOGW(TAG, "Skip wireless test module because NVS init failed");
    }
#endif

    ESP_LOGI(TAG, "System init task done");
    vTaskDelete(NULL);
}

void app_main(void)
{
    display_panel_t panel = {0};
    display_lvgl_t lvgl_port = {0};

    ESP_ERROR_CHECK(display_board_init());
    ESP_ERROR_CHECK(display_panel_init(&panel));
    ESP_ERROR_CHECK(display_board_set_backlight(100));
    ESP_ERROR_CHECK(display_lvgl_init(&panel, &lvgl_port));
    esp_err_t font_ret = display_font_init();
    if (font_ret != ESP_OK) {
        ESP_LOGW(TAG, "display font init failed, fallback to built-in patch font: %s", esp_err_to_name(font_ret));
    }

    ESP_LOGI(TAG, "Display LVGL integrated demo UI");
    display_lvgl_lock();
    example_lvgl_demo_ui(lvgl_port.display);
    display_lvgl_unlock();

    TaskHandle_t init_task = xTaskCreateStaticPinnedToCore(app_system_init_task,
                                                           "app_sys_init",
                                                           sizeof(s_system_init_task_stack) / sizeof(s_system_init_task_stack[0]),
                                                           NULL,
                                                           tskIDLE_PRIORITY + 4,
                                                           s_system_init_task_stack,
                                                           &s_system_init_task_tcb,
                                                           0);
    ESP_ERROR_CHECK(init_task != NULL ? ESP_OK : ESP_ERR_NO_MEM);
}
