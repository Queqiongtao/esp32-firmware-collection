/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "app_peripherals.h"

#include <inttypes.h>

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "test_audio.h"
#include "test_battery.h"
#include "test_rtc.h"
#include "test_sdcard.h"
#include "test_usb_role.h"
#include "test_wireless.h"

static const char *TAG = "app_peripherals";

typedef enum {
    APP_PERIPHERALS_NOT_STARTED = 0,
    APP_PERIPHERALS_STARTING,
    APP_PERIPHERALS_COMPLETE,
} app_peripherals_start_state_t;

static portMUX_TYPE s_start_lock = portMUX_INITIALIZER_UNLOCKED;
static app_peripherals_start_state_t s_start_state;
static app_peripherals_result_t s_start_result;

typedef esp_err_t (*app_peripheral_action_t)(void);

static esp_err_t app_peripherals_probe(const char *name,
                                       app_peripheral_action_t init,
                                       app_peripheral_action_t run,
                                       app_peripherals_result_t *result)
{
    const int64_t start_us = esp_timer_get_time();
    result->enabled_count++;

    esp_err_t init_ret = init();
    esp_err_t run_ret = ESP_OK;
    if (init_ret == ESP_OK && run != NULL) {
        run_ret = run();
    }

    const esp_err_t ret = init_ret != ESP_OK ? init_ret : run_ret;
    if (ret == ESP_OK) {
        result->ready_count++;
    } else {
        result->failed_count++;
        if (result->first_error == ESP_OK) {
            result->first_error = ret;
        }
    }

    ESP_LOGI(TAG,
             "capability=%s result=%s init=%s run=%s elapsed_ms=%" PRId64,
             name, ret == ESP_OK ? "ready" : "failed",
             esp_err_to_name(init_ret), esp_err_to_name(run_ret),
             (esp_timer_get_time() - start_us) / 1000);
    return ret;
}

esp_err_t app_peripherals_start(esp_err_t nvs_status, app_peripherals_result_t *out_result)
{
    portENTER_CRITICAL(&s_start_lock);
    app_peripherals_start_state_t start_state = s_start_state;
    app_peripherals_result_t cached_result = s_start_result;
    if (start_state == APP_PERIPHERALS_NOT_STARTED) {
        s_start_state = APP_PERIPHERALS_STARTING;
    }
    portEXIT_CRITICAL(&s_start_lock);

    if (start_state == APP_PERIPHERALS_COMPLETE) {
        if (out_result != NULL) {
            *out_result = cached_result;
        }
        ESP_LOGW(TAG, "startup already completed; returning cached result");
        return cached_result.first_error;
    }
    ESP_RETURN_ON_FALSE(start_state == APP_PERIPHERALS_NOT_STARTED,
                        ESP_ERR_INVALID_STATE, TAG,
                        "peripheral startup is already in progress");

    app_peripherals_result_t result = {
        .first_error = ESP_OK,
    };

#if CONFIG_EXAMPLE_ENABLE_TEST_SDCARD
    (void)app_peripherals_probe("sdcard", test_sdcard_init, test_sdcard_run, &result);
#endif

#if CONFIG_EXAMPLE_ENABLE_TEST_RTC
    (void)app_peripherals_probe("rtc", test_rtc_init, test_rtc_run, &result);
#endif

#if CONFIG_EXAMPLE_ENABLE_TEST_AUDIO
    (void)app_peripherals_probe("audio", test_audio_init, test_audio_run, &result);
#endif

#if CONFIG_EXAMPLE_ENABLE_TEST_BATTERY
    (void)app_peripherals_probe("battery", test_battery_init, test_battery_run, &result);
#endif

#if CONFIG_EXAMPLE_ENABLE_TEST_USB || CONFIG_EXAMPLE_ENABLE_TEST_UVC
    (void)app_peripherals_probe("usb_role", test_usb_role_init, NULL, &result);
#endif

#if CONFIG_EXAMPLE_ENABLE_TEST_WIRELESS
    if (nvs_status == ESP_OK) {
        (void)app_peripherals_probe("wireless", test_wireless_init, test_wireless_run, &result);
    } else {
        esp_err_t report_ret = test_wireless_report_dependency_failure(nvs_status);
        if (report_ret != ESP_OK) {
            ESP_LOGW(TAG, "record wireless NVS dependency failure failed: %s",
                     esp_err_to_name(report_ret));
        }
        result.enabled_count++;
        result.failed_count++;
        if (result.first_error == ESP_OK) {
            result.first_error = nvs_status;
        }
        ESP_LOGW(TAG, "capability=wireless result=skipped dependency=nvs error=%s",
                 esp_err_to_name(nvs_status));
    }
#else
    (void)nvs_status;
#endif

    ESP_LOGI(TAG,
             "startup_summary enabled=%" PRIu32 " ready=%" PRIu32
             " failed=%" PRIu32 " first_error=%s",
             result.enabled_count, result.ready_count, result.failed_count,
             esp_err_to_name(result.first_error));

    portENTER_CRITICAL(&s_start_lock);
    s_start_result = result;
    s_start_state = APP_PERIPHERALS_COMPLETE;
    portEXIT_CRITICAL(&s_start_lock);

    if (out_result != NULL) {
        *out_result = result;
    }
    return result.first_error;
}
