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
#include "test_config.h"

static const char *TAG = "test_battery";

typedef enum {
    BATTERY_CALI_SCHEME_NONE = 0,
    BATTERY_CALI_SCHEME_CURVE_FITTING,
    BATTERY_CALI_SCHEME_LINE_FITTING,
} battery_cali_scheme_t;

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_cali_handle;
static test_battery_status_t s_status;
static battery_cali_scheme_t s_cali_scheme;

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

static void battery_calibration_deinit(adc_cali_handle_t handle)
{
    switch (s_cali_scheme) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    case BATTERY_CALI_SCHEME_CURVE_FITTING:
        ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(handle));
        break;
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    case BATTERY_CALI_SCHEME_LINE_FITTING:
        ESP_ERROR_CHECK(adc_cali_delete_scheme_line_fitting(handle));
        break;
#endif
    case BATTERY_CALI_SCHEME_NONE:
    default:
        break;
    }
}

static esp_err_t battery_sample_once(void)
{
    int raw_sum = 0;
    int voltage_sum_mv = 0;

    for (int i = 0; i < TEST_BATTERY_SAMPLE_COUNT; ++i) {
        int raw = 0;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc_handle, TEST_BATTERY_ADC_CHANNEL, &raw),
                            TAG, "ADC oneshot read failed");
        raw_sum += raw;

        int adc_voltage_mv = 0;
        if (s_status.calibration_enabled) {
            ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali_handle, raw, &adc_voltage_mv),
                                TAG, "ADC raw-to-voltage failed");
        } else {
            adc_voltage_mv = raw * TEST_BATTERY_ADC_FALLBACK_REF_MV / TEST_BATTERY_ADC_RAW_FULL_SCALE;
        }
        voltage_sum_mv += adc_voltage_mv;
    }

    s_status.raw_average = raw_sum / TEST_BATTERY_SAMPLE_COUNT;
    s_status.adc_voltage_mv = voltage_sum_mv / TEST_BATTERY_SAMPLE_COUNT;
    s_status.battery_voltage_mv = battery_scale_to_mv(s_status.adc_voltage_mv);
    s_status.battery_percent = battery_percent_from_mv(s_status.battery_voltage_mv);
    s_status.sample_valid = true;
    s_status.sample_count++;
    s_status.last_update_ms = esp_timer_get_time() / 1000;
    s_status.last_error = ESP_OK;

    return ESP_OK;
}

esp_err_t test_battery_init(void)
{
    if (s_status.initialized) {
        return ESP_OK;
    }

    const adc_oneshot_unit_init_cfg_t adc_unit_cfg = {
        .unit_id = TEST_BATTERY_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_unit_cfg, &s_adc_handle),
                        TAG, "create ADC unit failed");

    const adc_oneshot_chan_cfg_t adc_chan_cfg = {
        .atten = TEST_BATTERY_ADC_ATTEN,
        .bitwidth = TEST_BATTERY_ADC_BITWIDTH,
    };
    esp_err_t ret = adc_oneshot_config_channel(s_adc_handle, TEST_BATTERY_ADC_CHANNEL, &adc_chan_cfg);
    if (ret != ESP_OK) {
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return ret;
    }

    s_status = (test_battery_status_t) {
        .initialized = true,
        .calibration_enabled = battery_calibration_init(&s_cali_handle),
        .sample_valid = false,
        .raw_average = 0,
        .adc_voltage_mv = 0,
        .battery_voltage_mv = 0,
        .battery_percent = 0,
        .sample_count = 0,
        .last_update_ms = 0,
        .last_error = ESP_OK,
    };

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

esp_err_t test_battery_run(void)
{
    if (!s_status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_status.sample_valid && (now_ms - s_status.last_update_ms) < TEST_BATTERY_UPDATE_PERIOD_MS) {
        return ESP_OK;
    }

    esp_err_t ret = battery_sample_once();
    if (ret != ESP_OK) {
        s_status.sample_valid = false;
        s_status.last_error = ret;
        ESP_LOGE(TAG, "battery sample failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

const test_battery_status_t *test_battery_get_status(void)
{
    return &s_status;
}

esp_err_t test_battery_deinit(void)
{
    if (!s_status.initialized) {
        return ESP_OK;
    }

    if (s_cali_handle != NULL) {
        battery_calibration_deinit(s_cali_handle);
        s_cali_handle = NULL;
        s_cali_scheme = BATTERY_CALI_SCHEME_NONE;
    }

    if (s_adc_handle != NULL) {
        ESP_RETURN_ON_ERROR(adc_oneshot_del_unit(s_adc_handle), TAG, "delete ADC unit failed");
        s_adc_handle = NULL;
    }

    s_status = (test_battery_status_t) {0};
    ESP_LOGI(TAG, "Battery ADC deinitialized");
    return ESP_OK;
}
