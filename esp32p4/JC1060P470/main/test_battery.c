/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "test_battery.h"

#include <inttypes.h>
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "test_config.h"

static const char *TAG = "test_battery";

#define TEST_BATTERY_TASK_STACK_SIZE             3072
#define TEST_BATTERY_TASK_PRIORITY               2
#define TEST_BATTERY_WORKER_EXITED_BIT           BIT0
#define TEST_BATTERY_WORKER_STOP_BIT             BIT1
#define TEST_BATTERY_STOP_TIMEOUT_MS             1000

typedef enum {
    BATTERY_CALI_SCHEME_NONE = 0,
    BATTERY_CALI_SCHEME_CURVE_FITTING,
    BATTERY_CALI_SCHEME_LINE_FITTING,
} battery_cali_scheme_t;

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_cali_handle;
static test_battery_status_t s_status = {
    .module = {
        .state = TEST_MODULE_STATE_UNINITIALIZED,
        .last_error = ESP_OK,
    },
};
static battery_cali_scheme_t s_cali_scheme;
static TaskHandle_t s_battery_task_handle;
static EventGroupHandle_t s_battery_worker_events;
static StaticEventGroup_t s_battery_worker_event_storage;
static portMUX_TYPE s_battery_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_lifecycle_mutex;
static StaticSemaphore_t s_lifecycle_mutex_storage;
static uint32_t s_generation;
static bool s_stopping;

static esp_err_t battery_lifecycle_enter(void)
{
    portENTER_CRITICAL(&s_battery_lock);
    if (s_lifecycle_mutex == NULL) {
        s_lifecycle_mutex = xSemaphoreCreateMutexStatic(&s_lifecycle_mutex_storage);
    }
    SemaphoreHandle_t mutex = s_lifecycle_mutex;
    portEXIT_CRITICAL(&s_battery_lock);

    ESP_RETURN_ON_FALSE(mutex != NULL, ESP_ERR_NO_MEM, TAG,
                        "create battery lifecycle mutex failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE,
                        ESP_ERR_INVALID_STATE, TAG,
                        "take battery lifecycle mutex failed");
    return ESP_OK;
}

static void battery_lifecycle_exit(void)
{
    if (s_lifecycle_mutex != NULL) {
        xSemaphoreGive(s_lifecycle_mutex);
    }
}

static bool battery_is_stopping(void)
{
    bool stopping;
    portENTER_CRITICAL(&s_battery_lock);
    stopping = s_stopping;
    portEXIT_CRITICAL(&s_battery_lock);
    return stopping;
}

static void battery_latch_error(esp_err_t error)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_battery_lock);
    s_status.last_error = error;
    s_status.module.state = TEST_MODULE_STATE_ERROR;
    s_status.module.last_error = error;
    s_status.module.updated_at_us = now_us;
    portEXIT_CRITICAL(&s_battery_lock);
}

static int battery_clamp_percent(int percent)
{
    if (percent < 0) {
        return 0;
    }
    if (percent > 100) {
        return 100;
    }
    return percent;
}

static int battery_scale_to_mv(int adc_voltage_mv)
{
    const int64_t numerator = (int64_t)adc_voltage_mv *
                              (TEST_BATTERY_DIVIDER_R_TOP_OHM + TEST_BATTERY_DIVIDER_R_BOTTOM_OHM);
    return (int)(numerator / TEST_BATTERY_DIVIDER_R_BOTTOM_OHM);
}

static int battery_percent_from_mv(int battery_voltage_mv)
{
    if (battery_voltage_mv <= TEST_BATTERY_EMPTY_MV) {
        return 0;
    }
    if (battery_voltage_mv >= TEST_BATTERY_FULL_MV) {
        return 100;
    }

    const int range_mv = TEST_BATTERY_FULL_MV - TEST_BATTERY_EMPTY_MV;
    const int delta_mv = battery_voltage_mv - TEST_BATTERY_EMPTY_MV;
    return battery_clamp_percent((delta_mv * 100) / range_mv);
}

static bool battery_calibration_init(adc_cali_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(out_handle != NULL, false, TAG, "out_handle is NULL");

    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = TEST_BATTERY_ADC_UNIT,
            .chan = TEST_BATTERY_ADC_CHANNEL,
            .atten = TEST_BATTERY_ADC_ATTEN,
            .bitwidth = TEST_BATTERY_ADC_BITWIDTH,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &handle);
        calibrated = (ret == ESP_OK);
        if (calibrated) {
            s_cali_scheme = BATTERY_CALI_SCHEME_CURVE_FITTING;
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_line_fitting_config_t cali_cfg = {
            .unit_id = TEST_BATTERY_ADC_UNIT,
            .atten = TEST_BATTERY_ADC_ATTEN,
            .bitwidth = TEST_BATTERY_ADC_BITWIDTH,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_cfg, &handle);
        calibrated = (ret == ESP_OK);
        if (calibrated) {
            s_cali_scheme = BATTERY_CALI_SCHEME_LINE_FITTING;
        }
    }
#endif

    *out_handle = handle;
    if (!calibrated) {
        if (ret == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "ADC calibration unavailable, fallback to raw voltage estimate");
        } else if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ADC calibration init failed: %s", esp_err_to_name(ret));
        }
    }

    return calibrated;
}

static esp_err_t battery_calibration_deinit(adc_cali_handle_t handle)
{
    switch (s_cali_scheme) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    case BATTERY_CALI_SCHEME_CURVE_FITTING:
        return adc_cali_delete_scheme_curve_fitting(handle);
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    case BATTERY_CALI_SCHEME_LINE_FITTING:
        return adc_cali_delete_scheme_line_fitting(handle);
#endif
    case BATTERY_CALI_SCHEME_NONE:
    default:
        return ESP_OK;
    }
}

static esp_err_t battery_release_hardware(void)
{
    if (s_cali_handle != NULL) {
        esp_err_t ret = battery_calibration_deinit(s_cali_handle);
        if (ret != ESP_OK) {
            return ret;
        }
        s_cali_handle = NULL;
        s_cali_scheme = BATTERY_CALI_SCHEME_NONE;
    }
    if (s_adc_handle != NULL) {
        esp_err_t ret = adc_oneshot_del_unit(s_adc_handle);
        if (ret != ESP_OK) {
            return ret;
        }
        s_adc_handle = NULL;
    }
    return ESP_OK;
}

static esp_err_t battery_sample_once(void)
{
    int raw_sum = 0;
    int voltage_sum_mv = 0;
    bool calibration_enabled;
    portENTER_CRITICAL(&s_battery_lock);
    calibration_enabled = s_status.calibration_enabled;
    portEXIT_CRITICAL(&s_battery_lock);

    for (int i = 0; i < TEST_BATTERY_SAMPLE_COUNT; ++i) {
        int raw = 0;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc_handle, TEST_BATTERY_ADC_CHANNEL, &raw),
                            TAG, "ADC oneshot read failed");
        raw_sum += raw;

        int adc_voltage_mv = 0;
        if (calibration_enabled) {
            ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali_handle, raw, &adc_voltage_mv),
                                TAG, "ADC raw-to-voltage failed");
        } else {
            adc_voltage_mv = raw * TEST_BATTERY_ADC_FALLBACK_REF_MV / TEST_BATTERY_ADC_RAW_FULL_SCALE;
        }
        voltage_sum_mv += adc_voltage_mv;
    }

    int raw_average = raw_sum / TEST_BATTERY_SAMPLE_COUNT;
    int adc_voltage_mv = voltage_sum_mv / TEST_BATTERY_SAMPLE_COUNT;
    int battery_voltage_mv = battery_scale_to_mv(adc_voltage_mv);
    int battery_percent = battery_percent_from_mv(battery_voltage_mv);
    int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_battery_lock);
    s_status.raw_average = raw_average;
    s_status.adc_voltage_mv = adc_voltage_mv;
    s_status.battery_voltage_mv = battery_voltage_mv;
    s_status.battery_percent = battery_percent;
    s_status.sample_valid = true;
    s_status.sample_count++;
    s_status.last_update_ms = now_us / 1000;
    s_status.last_error = ESP_OK;
    s_status.module.state = TEST_MODULE_STATE_RUNNING;
    s_status.module.last_error = ESP_OK;
    s_status.module.updated_at_us = now_us;
    portEXIT_CRITICAL(&s_battery_lock);

    return ESP_OK;
}

static void battery_periodic_task(void *arg)
{
    (void)arg;

    while (!battery_is_stopping()) {
        esp_err_t ret = battery_sample_once();
        if (ret != ESP_OK) {
            portENTER_CRITICAL(&s_battery_lock);
            s_status.sample_valid = false;
            portEXIT_CRITICAL(&s_battery_lock);
            battery_latch_error(ret);
            ESP_LOGE(TAG, "battery sample failed: %s", esp_err_to_name(ret));
        }

        (void)xEventGroupWaitBits(s_battery_worker_events,
                                  TEST_BATTERY_WORKER_STOP_BIT,
                                  pdTRUE,
                                  pdFALSE,
                                  pdMS_TO_TICKS(TEST_BATTERY_UPDATE_PERIOD_MS));
        if (battery_is_stopping()) {
            break;
        }
    }

    EventGroupHandle_t events;
    portENTER_CRITICAL(&s_battery_lock);
    s_status.running = false;
    s_battery_task_handle = NULL;
    events = s_battery_worker_events;
    portEXIT_CRITICAL(&s_battery_lock);
    if (events != NULL) {
        xEventGroupSetBits(events, TEST_BATTERY_WORKER_EXITED_BIT);
    }
    vTaskDelete(NULL);
}

static esp_err_t test_battery_init_locked(void)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_battery_lock);
    bool initialized = s_status.initialized;
    bool worker_exists = s_battery_task_handle != NULL;
    bool stopping = s_stopping;
    test_module_state_t module_state = s_status.module.state;
    if (!initialized && !worker_exists) {
        if (!stopping && module_state != TEST_MODULE_STATE_INITIALIZING &&
            module_state != TEST_MODULE_STATE_STOPPING) {
            s_stopping = false;
            s_status.module.state = TEST_MODULE_STATE_INITIALIZING;
            s_status.module.last_error = ESP_OK;
            s_status.module.updated_at_us = now_us;
        }
    }
    portEXIT_CRITICAL(&s_battery_lock);
    if (initialized) {
        return !stopping &&
               (module_state == TEST_MODULE_STATE_READY ||
                module_state == TEST_MODULE_STATE_RUNNING) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(!worker_exists && !stopping &&
                        module_state != TEST_MODULE_STATE_INITIALIZING &&
                        module_state != TEST_MODULE_STATE_STOPPING,
                        ESP_ERR_INVALID_STATE, TAG, "battery lifecycle is busy");

    esp_err_t ret = battery_release_hardware();
    if (ret != ESP_OK) {
        battery_latch_error(ret);
        return ret;
    }

    const adc_oneshot_unit_init_cfg_t adc_unit_cfg = {
        .unit_id = TEST_BATTERY_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ret = adc_oneshot_new_unit(&adc_unit_cfg, &s_adc_handle);
    if (ret != ESP_OK) {
        battery_latch_error(ret);
        return ret;
    }

    const adc_oneshot_chan_cfg_t adc_chan_cfg = {
        .atten = TEST_BATTERY_ADC_ATTEN,
        .bitwidth = TEST_BATTERY_ADC_BITWIDTH,
    };
    ret = adc_oneshot_config_channel(s_adc_handle, TEST_BATTERY_ADC_CHANNEL, &adc_chan_cfg);
    if (ret != ESP_OK) {
        esp_err_t cleanup_ret = adc_oneshot_del_unit(s_adc_handle);
        if (cleanup_ret == ESP_OK) {
            s_adc_handle = NULL;
        } else {
            ESP_LOGE(TAG, "delete ADC unit after config failure failed: %s", esp_err_to_name(cleanup_ret));
        }
        battery_latch_error(ret);
        return ret;
    }

    s_cali_scheme = BATTERY_CALI_SCHEME_NONE;
    bool calibration_enabled = battery_calibration_init(&s_cali_handle);
    if (s_battery_worker_events == NULL) {
        s_battery_worker_events = xEventGroupCreateStatic(&s_battery_worker_event_storage);
    }

    now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_battery_lock);
    s_generation++;
    s_status = (test_battery_status_t) {
        .module = {
            .state = TEST_MODULE_STATE_READY,
            .last_error = ESP_OK,
            .generation = s_generation,
            .updated_at_us = now_us,
        },
        .initialized = true,
        .running = false,
        .calibration_enabled = calibration_enabled,
        .sample_valid = false,
        .raw_average = 0,
        .adc_voltage_mv = 0,
        .battery_voltage_mv = 0,
        .battery_percent = 0,
        .sample_count = 0,
        .last_update_ms = 0,
        .last_error = ESP_OK,
    };
    portEXIT_CRITICAL(&s_battery_lock);

    ESP_LOGI(TAG,
             "Battery ADC initialized on GPIO%d (ADC unit %d channel %d), divider=%d/%d, calibration=%s",
             TEST_BATTERY_ADC_GPIO,
             TEST_BATTERY_ADC_UNIT + 1,
             TEST_BATTERY_ADC_CHANNEL,
             TEST_BATTERY_DIVIDER_R_TOP_OHM,
             TEST_BATTERY_DIVIDER_R_BOTTOM_OHM,
             s_status.calibration_enabled ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t test_battery_init(void)
{
    ESP_RETURN_ON_ERROR(battery_lifecycle_enter(), TAG,
                        "enter battery lifecycle failed");
    esp_err_t ret = test_battery_init_locked();
    battery_lifecycle_exit();
    return ret;
}

static esp_err_t test_battery_run_locked(void)
{
    portENTER_CRITICAL(&s_battery_lock);
    bool initialized = s_status.initialized;
    bool running = s_status.running;
    bool stopping = s_stopping;
    portEXIT_CRITICAL(&s_battery_lock);
    if (!initialized || stopping) {
        return ESP_ERR_INVALID_STATE;
    }
    if (running) {
        return ESP_OK;
    }

    xEventGroupClearBits(s_battery_worker_events,
                         TEST_BATTERY_WORKER_EXITED_BIT | TEST_BATTERY_WORKER_STOP_BIT);
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_battery_lock);
    s_status.running = true;
    s_status.module.state = TEST_MODULE_STATE_RUNNING;
    s_status.module.last_error = ESP_OK;
    s_status.module.updated_at_us = now_us;
    portEXIT_CRITICAL(&s_battery_lock);

    BaseType_t created = xTaskCreate(battery_periodic_task,
                                     "battery_test",
                                     TEST_BATTERY_TASK_STACK_SIZE,
                                     NULL,
                                     TEST_BATTERY_TASK_PRIORITY,
                                     &s_battery_task_handle);
    if (created != pdPASS) {
        portENTER_CRITICAL(&s_battery_lock);
        s_status.running = false;
        s_battery_task_handle = NULL;
        portEXIT_CRITICAL(&s_battery_lock);
        battery_latch_error(ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Battery periodic worker started, interval=%d ms", TEST_BATTERY_UPDATE_PERIOD_MS);
    return ESP_OK;
}

esp_err_t test_battery_run(void)
{
    ESP_RETURN_ON_ERROR(battery_lifecycle_enter(), TAG,
                        "enter battery lifecycle failed");
    esp_err_t ret = test_battery_run_locked();
    battery_lifecycle_exit();
    return ret;
}

esp_err_t test_battery_get_status(test_battery_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != NULL, ESP_ERR_INVALID_ARG, TAG, "out_status is NULL");

    portENTER_CRITICAL(&s_battery_lock);
    *out_status = s_status;
    portEXIT_CRITICAL(&s_battery_lock);
    return ESP_OK;
}

static esp_err_t test_battery_deinit_locked(void)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_battery_lock);
    bool worker_exists = s_battery_task_handle != NULL;
    s_stopping = true;
    s_status.module.state = TEST_MODULE_STATE_STOPPING;
    s_status.module.updated_at_us = now_us;
    portEXIT_CRITICAL(&s_battery_lock);

    if (worker_exists) {
        xEventGroupSetBits(s_battery_worker_events, TEST_BATTERY_WORKER_STOP_BIT);
        EventBits_t bits = xEventGroupWaitBits(s_battery_worker_events,
                                               TEST_BATTERY_WORKER_EXITED_BIT,
                                               pdTRUE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(TEST_BATTERY_STOP_TIMEOUT_MS));
        if ((bits & TEST_BATTERY_WORKER_EXITED_BIT) == 0) {
            ESP_LOGE(TAG, "Battery worker did not acknowledge stop within %d ms",
                     TEST_BATTERY_STOP_TIMEOUT_MS);
            battery_latch_error(ESP_ERR_TIMEOUT);
            return ESP_ERR_TIMEOUT;
        }
    }

    esp_err_t ret = battery_release_hardware();
    if (ret != ESP_OK) {
        battery_latch_error(ret);
        return ret;
    }
    now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_battery_lock);
    s_status = (test_battery_status_t) {
        .module = {
            .state = TEST_MODULE_STATE_UNINITIALIZED,
            .last_error = ESP_OK,
            .generation = s_generation,
            .updated_at_us = now_us,
        },
    };
    s_stopping = false;
    portEXIT_CRITICAL(&s_battery_lock);
    ESP_LOGI(TAG, "Battery ADC deinitialized");
    return ESP_OK;
}

esp_err_t test_battery_deinit(void)
{
    ESP_RETURN_ON_ERROR(battery_lifecycle_enter(), TAG,
                        "enter battery lifecycle failed");
    esp_err_t ret = test_battery_deinit_locked();
    battery_lifecycle_exit();
    return ret;
}
