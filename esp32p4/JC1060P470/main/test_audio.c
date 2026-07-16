/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "test_audio.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "board_i2c.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "test_config.h"

static const char *TAG = "test_audio";

#define TEST_AUDIO_ES8311_SYSTEM_REG14         0x14
#define TEST_AUDIO_ES8311_ADC_REG15            0x15
#define TEST_AUDIO_ES8311_ADC_REG16            0x16
#define TEST_AUDIO_ES8311_ADC_REG17            0x17
#define TEST_AUDIO_ES8311_GPIO_REG44           0x44
#define TEST_AUDIO_ACTION_QUEUE_LENGTH         1
#define TEST_AUDIO_WORKER_EXITED_BIT           BIT0
#define TEST_AUDIO_STOP_TIMEOUT_MS             2500

extern const uint8_t canon_pcm_start[] asm("_binary_canon_pcm_start");
extern const uint8_t canon_pcm_end[]   asm("_binary_canon_pcm_end");

typedef enum {
    TEST_AUDIO_ACTION_NONE = 0,
    TEST_AUDIO_ACTION_MUSIC,
    TEST_AUDIO_ACTION_RECORD,
    TEST_AUDIO_ACTION_PLAYBACK,
    TEST_AUDIO_ACTION_STOP,
} test_audio_action_t;

static i2s_chan_handle_t s_tx_handle = NULL;
static i2s_chan_handle_t s_rx_handle = NULL;
static bool s_tx_enabled = false;
static bool s_rx_enabled = false;
static esp_codec_dev_handle_t s_codec_dev = NULL;
static const audio_codec_ctrl_if_t *s_codec_ctrl_if = NULL;
static const audio_codec_data_if_t *s_codec_data_if = NULL;
static const audio_codec_gpio_if_t *s_codec_gpio_if = NULL;
static const audio_codec_if_t *s_codec_if = NULL;
static TaskHandle_t s_audio_task_handle = NULL;
static QueueHandle_t s_action_queue = NULL;
static EventGroupHandle_t s_worker_events = NULL;
static StaticEventGroup_t s_worker_event_storage;
static portMUX_TYPE s_audio_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_lifecycle_mutex = NULL;
static StaticSemaphore_t s_lifecycle_mutex_storage;
static uint8_t *s_record_buffer = NULL;
static size_t s_record_size = 0;
static uint16_t s_record_peak = 0;
static size_t s_record_nonzero_samples = 0;
static test_audio_state_t s_state = TEST_AUDIO_STATE_UNINITIALIZED;
static esp_err_t s_last_error = ESP_OK;
static test_module_meta_t s_module = {
    .state = TEST_MODULE_STATE_UNINITIALIZED,
    .last_error = ESP_OK,
};
static uint32_t s_generation = 0;
static bool s_initialized = false;
static bool s_running = false;
static bool s_stopping = false;

static esp_err_t test_audio_lifecycle_enter(void)
{
    portENTER_CRITICAL(&s_audio_lock);
    if (s_lifecycle_mutex == NULL) {
        s_lifecycle_mutex = xSemaphoreCreateMutexStatic(&s_lifecycle_mutex_storage);
    }
    SemaphoreHandle_t mutex = s_lifecycle_mutex;
    portEXIT_CRITICAL(&s_audio_lock);

    ESP_RETURN_ON_FALSE(mutex != NULL, ESP_ERR_NO_MEM, TAG,
                        "create audio lifecycle mutex failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE,
                        ESP_ERR_INVALID_STATE, TAG,
                        "take audio lifecycle mutex failed");
    return ESP_OK;
}

static void test_audio_lifecycle_exit(void)
{
    if (s_lifecycle_mutex != NULL) {
        xSemaphoreGive(s_lifecycle_mutex);
    }
}

static void test_audio_log_codec_reg(const char *name, int reg)
{
    int value = 0;
    int ret = esp_codec_dev_read_reg(s_codec_dev, reg, &value);
    if (ret == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "ES8311 %s(0x%02x)=0x%02x", name, reg, value & 0xff);
    } else {
        ESP_LOGW(TAG, "ES8311 read %s(0x%02x) failed: %d", name, reg, ret);
    }
}

static void test_audio_dump_input_regs(const char *stage)
{
    ESP_LOGI(TAG, "Dump ES8311 input regs at %s", stage);
    test_audio_log_codec_reg("ADC_REG15", TEST_AUDIO_ES8311_ADC_REG15);
    test_audio_log_codec_reg("ADC_REG16", TEST_AUDIO_ES8311_ADC_REG16);
    test_audio_log_codec_reg("ADC_REG17", TEST_AUDIO_ES8311_ADC_REG17);
    test_audio_log_codec_reg("SYSTEM_REG14", TEST_AUDIO_ES8311_SYSTEM_REG14);
    test_audio_log_codec_reg("GPIO_REG44", TEST_AUDIO_ES8311_GPIO_REG44);
}

static void test_audio_update_record_stats(const uint8_t *data, size_t bytes,
                                           uint16_t *peak, size_t *nonzero_samples)
{
    const int16_t *samples = (const int16_t *)data;
    size_t sample_count = bytes / sizeof(int16_t);

    for (size_t i = 0; i < sample_count; ++i) {
        int32_t sample = samples[i];
        uint32_t amplitude = (sample == INT16_MIN) ? (uint32_t)INT16_MAX + 1U : (uint32_t)(sample < 0 ? -sample : sample);
        if (amplitude > *peak) {
            *peak = (uint16_t)(amplitude > UINT16_MAX ? UINT16_MAX : amplitude);
        }
        if (sample != 0) {
            (*nonzero_samples)++;
        }
    }
}

static const char *test_audio_describe_input_level(uint16_t peak, size_t nonzero_samples)
{
    if (peak == 0 || nonzero_samples == 0) {
        return "silent";
    }
    if (peak < 256) {
        return "weak";
    }
    return "active";
}

static void test_audio_set_state(test_audio_state_t state)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_audio_lock);
    s_state = state;
    s_module.updated_at_us = now_us;
    portEXIT_CRITICAL(&s_audio_lock);
}

static bool test_audio_is_stopping(void)
{
    bool stopping;

    portENTER_CRITICAL(&s_audio_lock);
    stopping = s_stopping;
    portEXIT_CRITICAL(&s_audio_lock);
    return stopping;
}

static void test_audio_set_module_state(test_module_state_t state, esp_err_t error)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_audio_lock);
    s_module.state = state;
    s_module.last_error = error;
    s_module.updated_at_us = now_us;
    s_last_error = error;
    portEXIT_CRITICAL(&s_audio_lock);
}

static void test_audio_latch_error(esp_err_t err)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_audio_lock);
    s_state = TEST_AUDIO_STATE_ERROR;
    s_last_error = err;
    s_module.state = TEST_MODULE_STATE_ERROR;
    s_module.last_error = err;
    s_module.updated_at_us = now_us;
    portEXIT_CRITICAL(&s_audio_lock);
}

static test_audio_state_t test_audio_get_idle_state_after_action(void)
{
    size_t record_size;
    portENTER_CRITICAL(&s_audio_lock);
    record_size = s_record_size;
    portEXIT_CRITICAL(&s_audio_lock);
    return record_size > 0 ? TEST_AUDIO_STATE_RECORDED : TEST_AUDIO_STATE_IDLE;
}

static esp_err_t test_audio_release_i2s(void)
{
    esp_err_t first_error = ESP_OK;

    if (s_rx_handle != NULL) {
        if (s_rx_enabled) {
            esp_err_t ret = i2s_channel_disable(s_rx_handle);
            if (ret == ESP_OK) {
                s_rx_enabled = false;
            } else if (first_error == ESP_OK) {
                first_error = ret;
            }
        }
        esp_err_t ret = i2s_del_channel(s_rx_handle);
        if (ret == ESP_OK) {
            s_rx_handle = NULL;
            s_rx_enabled = false;
        } else if (first_error == ESP_OK) {
            first_error = ret;
        }
    }

    if (s_tx_handle != NULL) {
        if (s_tx_enabled) {
            esp_err_t ret = i2s_channel_disable(s_tx_handle);
            if (ret == ESP_OK) {
                s_tx_enabled = false;
            } else if (first_error == ESP_OK) {
                first_error = ret;
            }
        }
        esp_err_t ret = i2s_del_channel(s_tx_handle);
        if (ret == ESP_OK) {
            s_tx_handle = NULL;
            s_tx_enabled = false;
        } else if (first_error == ESP_OK) {
            first_error = ret;
        }
    }

    return first_error;
}

static esp_err_t test_audio_release_codec(void)
{
    esp_err_t first_error = ESP_OK;

    if (s_codec_dev != NULL) {
        int ret = esp_codec_dev_close(s_codec_dev);
        if (ret != ESP_CODEC_DEV_OK) {
            first_error = ESP_FAIL;
            ESP_LOGW(TAG, "close codec device failed: %d", ret);
        }
        esp_codec_dev_delete(s_codec_dev);
        s_codec_dev = NULL;
    }
    if (s_codec_if != NULL) {
        int ret = audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
        if (ret != ESP_CODEC_DEV_OK && first_error == ESP_OK) {
            first_error = ESP_FAIL;
            ESP_LOGW(TAG, "delete codec interface failed: %d", ret);
        }
    }
    if (s_codec_gpio_if != NULL) {
        int ret = audio_codec_delete_gpio_if(s_codec_gpio_if);
        s_codec_gpio_if = NULL;
        if (ret != ESP_CODEC_DEV_OK && first_error == ESP_OK) {
            first_error = ESP_FAIL;
            ESP_LOGW(TAG, "delete codec GPIO interface failed: %d", ret);
        }
    }
    if (s_codec_data_if != NULL) {
        int ret = audio_codec_delete_data_if(s_codec_data_if);
        s_codec_data_if = NULL;
        if (ret != ESP_CODEC_DEV_OK && first_error == ESP_OK) {
            first_error = ESP_FAIL;
            ESP_LOGW(TAG, "delete codec data interface failed: %d", ret);
        }
    }
    if (s_codec_ctrl_if != NULL) {
        int ret = audio_codec_delete_ctrl_if(s_codec_ctrl_if);
        s_codec_ctrl_if = NULL;
        if (ret != ESP_CODEC_DEV_OK && first_error == ESP_OK) {
            first_error = ESP_FAIL;
            ESP_LOGW(TAG, "delete codec control interface failed: %d", ret);
        }
    }

    return first_error;
}

static esp_err_t test_audio_i2s_driver_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(TEST_AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle), TAG, "create I2S channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(TEST_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = TEST_AUDIO_PIN_MCLK,
            .bclk = TEST_AUDIO_PIN_BCLK,
            .ws = TEST_AUDIO_PIN_WS,
            .dout = TEST_AUDIO_PIN_DOUT,
            .din = TEST_AUDIO_PIN_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = TEST_AUDIO_MCLK_MULTIPLE;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg), TAG, "init I2S tx std mode failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &std_cfg), TAG, "init I2S rx std mode failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "enable I2S tx failed");
    s_tx_enabled = true;
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "enable I2S rx failed");
    s_rx_enabled = true;
    return ESP_OK;
}

static esp_err_t test_audio_codec_init(void)
{
    i2c_master_bus_handle_t i2c_bus_handle = NULL;
    i2c_port_num_t i2c_port = I2C_NUM_0;
    ESP_RETURN_ON_ERROR(board_i2c_get_bus(&i2c_bus_handle, &i2c_port),
                        TAG, "get shared I2C bus handle failed");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = i2c_port,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus_handle,
    };
    s_codec_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_codec_ctrl_if, ESP_FAIL, TAG, "create I2C control interface failed");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = TEST_AUDIO_I2S_NUM,
        .rx_handle = s_rx_handle,
        .tx_handle = s_tx_handle,
    };
    s_codec_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_codec_data_if, ESP_FAIL, TAG, "create I2S data interface failed");

    s_codec_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_codec_gpio_if, ESP_FAIL, TAG, "create GPIO interface failed");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = s_codec_ctrl_if,
        .gpio_if = s_codec_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .master_mode = false,
        .use_mclk = TEST_AUDIO_PIN_MCLK >= 0,
        .pa_pin = TEST_AUDIO_PIN_PA,
        .pa_reverted = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .mclk_div = TEST_AUDIO_MCLK_MULTIPLE,
    };
    s_codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(s_codec_if, ESP_FAIL, TAG, "create ES8311 codec interface failed");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_codec_if,
        .data_if = s_codec_data_if,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec_dev, ESP_FAIL, TAG, "create codec device failed");

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = TEST_AUDIO_CHANNELS,
        .channel_mask = 0x03,
        .sample_rate = TEST_AUDIO_SAMPLE_RATE,
        .mclk_multiple = TEST_AUDIO_MCLK_MULTIPLE,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_codec_dev, &sample_cfg), TAG, "open codec device failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_codec_dev, TEST_AUDIO_VOICE_VOLUME), TAG, "set output volume failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_codec_dev, TEST_AUDIO_MIC_GAIN), TAG, "set input gain failed");
    test_audio_dump_input_regs("codec_init");
    return ESP_OK;
}

static esp_err_t test_audio_record_once(void)
{
    if (s_record_buffer == NULL) {
        s_record_buffer = heap_caps_malloc(TEST_AUDIO_RECORD_BUF_SIZE,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(s_record_buffer, ESP_ERR_NO_MEM, TAG,
                            "allocate %d-byte PSRAM record buffer failed",
                            TEST_AUDIO_RECORD_BUF_SIZE);
    }

    test_audio_set_state(TEST_AUDIO_STATE_RECORDING);
    memset(s_record_buffer, 0, TEST_AUDIO_RECORD_BUF_SIZE);
    size_t record_size = 0;
    uint16_t record_peak = 0;
    size_t nonzero_samples = 0;
    portENTER_CRITICAL(&s_audio_lock);
    s_record_size = record_size;
    s_record_peak = record_peak;
    s_record_nonzero_samples = nonzero_samples;
    portEXIT_CRITICAL(&s_audio_lock);

    ESP_LOGI(TAG, "Start recording: %d seconds, target=%d bytes",
             TEST_AUDIO_RECORD_SECONDS, TEST_AUDIO_RECORD_BUF_SIZE);
    test_audio_dump_input_regs("record_start");

    while (!test_audio_is_stopping() && record_size < TEST_AUDIO_RECORD_BUF_SIZE) {
        size_t bytes_to_read = TEST_AUDIO_RECV_BUF_SIZE;
        size_t remaining = TEST_AUDIO_RECORD_BUF_SIZE - record_size;
        if (bytes_to_read > remaining) {
            bytes_to_read = remaining;
        }

        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_rx_handle,
                                         s_record_buffer + record_size,
                                         bytes_to_read,
                                         &bytes_read,
                                         pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            if (test_audio_is_stopping() && ret == ESP_ERR_TIMEOUT) {
                break;
            }
            ESP_LOGE(TAG, "i2s read failed during record: %s", esp_err_to_name(ret));
            test_audio_latch_error(ret);
            return ret;
        }
        if (bytes_read == 0) {
            continue;
        }
        test_audio_update_record_stats(s_record_buffer + record_size, bytes_read,
                                       &record_peak, &nonzero_samples);
        record_size += bytes_read;
        portENTER_CRITICAL(&s_audio_lock);
        s_record_size = record_size;
        s_record_peak = record_peak;
        s_record_nonzero_samples = nonzero_samples;
        portEXIT_CRITICAL(&s_audio_lock);
    }

    if (test_audio_is_stopping()) {
        return ESP_OK;
    }

    test_audio_set_state(TEST_AUDIO_STATE_RECORDED);
    ESP_LOGI(TAG, "Recording finished, valid=%d bytes, peak=%u, nonzero_samples=%u, input=%s",
             (int)record_size, (unsigned int)record_peak, (unsigned int)nonzero_samples,
             test_audio_describe_input_level(record_peak, nonzero_samples));
    return ESP_OK;
}

static esp_err_t test_audio_playback_once(void)
{
    size_t record_size;
    portENTER_CRITICAL(&s_audio_lock);
    record_size = s_record_size;
    portEXIT_CRITICAL(&s_audio_lock);
    ESP_RETURN_ON_FALSE(s_record_buffer && record_size > 0, ESP_ERR_INVALID_STATE, TAG, "no recorded audio");

    test_audio_set_state(TEST_AUDIO_STATE_PLAYING);
    ESP_LOGI(TAG, "Start playback: valid=%d bytes", (int)record_size);

    size_t offset = 0;
    while (!test_audio_is_stopping() && offset < record_size) {
        size_t bytes_write = 0;
        size_t bytes_to_write = record_size - offset;
        if (bytes_to_write > TEST_AUDIO_RECV_BUF_SIZE) {
            bytes_to_write = TEST_AUDIO_RECV_BUF_SIZE;
        }

        esp_err_t ret = i2s_channel_write(s_tx_handle,
                                          s_record_buffer + offset,
                                          bytes_to_write,
                                          &bytes_write,
                                          pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            if (test_audio_is_stopping() && ret == ESP_ERR_TIMEOUT) {
                break;
            }
            ESP_LOGE(TAG, "i2s write failed during playback: %s", esp_err_to_name(ret));
            test_audio_latch_error(ret);
            return ret;
        }
        if (bytes_write == 0) {
            continue;
        }
        offset += bytes_write;
    }

    if (test_audio_is_stopping()) {
        return ESP_OK;
    }

    test_audio_set_state(TEST_AUDIO_STATE_RECORDED);
    ESP_LOGI(TAG, "Playback finished");
    return ESP_OK;
}

static esp_err_t test_audio_play_music_once(void)
{
    const uint8_t *music_data = canon_pcm_start;
    size_t remaining = (size_t)(canon_pcm_end - canon_pcm_start);

    test_audio_set_state(TEST_AUDIO_STATE_MUSIC_PLAYING);
    ESP_LOGI(TAG, "Start music playback: %d bytes", (int)remaining);

    while (!test_audio_is_stopping() && remaining > 0) {
        size_t bytes_write = 0;
        size_t bytes_to_write = remaining;
        if (bytes_to_write > TEST_AUDIO_RECV_BUF_SIZE) {
            bytes_to_write = TEST_AUDIO_RECV_BUF_SIZE;
        }

        esp_err_t ret = i2s_channel_write(s_tx_handle, music_data,
                                          bytes_to_write, &bytes_write, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            if (test_audio_is_stopping() && ret == ESP_ERR_TIMEOUT) {
                break;
            }
            ESP_LOGE(TAG, "i2s write failed during music playback: %s", esp_err_to_name(ret));
            test_audio_latch_error(ret);
            return ret;
        }
        if (bytes_write == 0) {
            continue;
        }
        music_data += bytes_write;
        remaining -= bytes_write;
    }

    if (test_audio_is_stopping()) {
        return ESP_OK;
    }

    test_audio_set_state(test_audio_get_idle_state_after_action());
    ESP_LOGI(TAG, "Music playback finished");
    return ESP_OK;
}

static void test_audio_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Audio action service ready");
    while (!test_audio_is_stopping()) {
        test_audio_action_t action = TEST_AUDIO_ACTION_NONE;
        if (xQueueReceive(s_action_queue, &action, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (action == TEST_AUDIO_ACTION_STOP || test_audio_is_stopping()) {
            break;
        }

        esp_err_t ret = ESP_OK;
        switch (action) {
        case TEST_AUDIO_ACTION_MUSIC:
            ret = test_audio_play_music_once();
            break;
        case TEST_AUDIO_ACTION_RECORD:
            ret = test_audio_record_once();
            break;
        case TEST_AUDIO_ACTION_PLAYBACK:
            ret = test_audio_playback_once();
            break;
        case TEST_AUDIO_ACTION_NONE:
        case TEST_AUDIO_ACTION_STOP:
        default:
            continue;
        }

        if (ret != ESP_OK && !test_audio_is_stopping()) {
            ESP_LOGE(TAG, "Audio action failed: %s", esp_err_to_name(ret));
            test_audio_latch_error(ret);
            continue;
        }
        if (!test_audio_is_stopping()) {
            test_audio_set_state(test_audio_get_idle_state_after_action());
            test_audio_set_module_state(TEST_MODULE_STATE_RUNNING, ESP_OK);
        }
    }

    EventGroupHandle_t events;
    portENTER_CRITICAL(&s_audio_lock);
    s_running = false;
    s_audio_task_handle = NULL;
    events = s_worker_events;
    portEXIT_CRITICAL(&s_audio_lock);
    if (events != NULL) {
        xEventGroupSetBits(events, TEST_AUDIO_WORKER_EXITED_BIT);
    }
    vTaskDelete(NULL);
}

static esp_err_t test_audio_request_action_locked(test_audio_action_t action)
{
    test_audio_state_t next_state;
    switch (action) {
    case TEST_AUDIO_ACTION_RECORD:
        next_state = TEST_AUDIO_STATE_RECORDING;
        break;
    case TEST_AUDIO_ACTION_PLAYBACK:
        next_state = TEST_AUDIO_STATE_PLAYING;
        break;
    case TEST_AUDIO_ACTION_MUSIC:
        next_state = TEST_AUDIO_STATE_MUSIC_PLAYING;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    int64_t now_us = esp_timer_get_time();
    test_audio_state_t previous_state;
    portENTER_CRITICAL(&s_audio_lock);
    if (!s_initialized || !s_running || s_stopping || s_action_queue == NULL) {
        portEXIT_CRITICAL(&s_audio_lock);
        return ESP_ERR_INVALID_STATE;
    }

    previous_state = s_state;
    bool ready = previous_state == TEST_AUDIO_STATE_IDLE ||
                 previous_state == TEST_AUDIO_STATE_RECORDED ||
                 previous_state == TEST_AUDIO_STATE_ERROR;
    if (action == TEST_AUDIO_ACTION_PLAYBACK) {
        ready = previous_state == TEST_AUDIO_STATE_RECORDED && s_record_size > 0;
    }
    if (!ready) {
        portEXIT_CRITICAL(&s_audio_lock);
        ESP_LOGW(TAG, "audio busy or action not ready, state=%d action=%d", previous_state, action);
        return ESP_ERR_INVALID_STATE;
    }

    s_state = next_state;
    s_last_error = ESP_OK;
    s_module.state = TEST_MODULE_STATE_RUNNING;
    s_module.last_error = ESP_OK;
    s_module.updated_at_us = now_us;
    portEXIT_CRITICAL(&s_audio_lock);

    if (xQueueSend(s_action_queue, &action, 0) != pdTRUE) {
        /* Keep the public action state and module metadata aligned when the
         * request was rejected before ownership reached the worker. */
        test_audio_latch_error(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t test_audio_request_action(test_audio_action_t action)
{
    ESP_RETURN_ON_ERROR(test_audio_lifecycle_enter(), TAG,
                        "enter audio lifecycle failed");
    esp_err_t ret = test_audio_request_action_locked(action);
    test_audio_lifecycle_exit();
    return ret;
}

static esp_err_t test_audio_init_locked(void)
{
    ESP_LOGI(TAG, "Initializing audio (I2S + ES8311 codec)");

    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_audio_lock);
    bool initialized = s_initialized;
    bool worker_exists = s_audio_task_handle != NULL;
    bool stopping = s_stopping;
    test_module_state_t module_state = s_module.state;
    if (!initialized && !worker_exists) {
        if (!stopping && module_state != TEST_MODULE_STATE_INITIALIZING &&
            module_state != TEST_MODULE_STATE_STOPPING) {
            s_stopping = false;
            s_module.state = TEST_MODULE_STATE_INITIALIZING;
            s_module.last_error = ESP_OK;
            s_module.updated_at_us = now_us;
        }
    }
    portEXIT_CRITICAL(&s_audio_lock);
    if (initialized) {
        return !stopping &&
               (module_state == TEST_MODULE_STATE_READY ||
                module_state == TEST_MODULE_STATE_RUNNING) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(!worker_exists && !stopping &&
                        module_state != TEST_MODULE_STATE_INITIALIZING &&
                        module_state != TEST_MODULE_STATE_STOPPING,
                        ESP_ERR_INVALID_STATE, TAG, "audio lifecycle is busy");

    esp_err_t ret = test_audio_release_codec();
    esp_err_t i2s_ret = test_audio_release_i2s();
    if (ret == ESP_OK) {
        ret = i2s_ret;
    }
    if (ret != ESP_OK) {
        test_audio_latch_error(ret);
        return ret;
    }

    s_action_queue = xQueueCreate(TEST_AUDIO_ACTION_QUEUE_LENGTH, sizeof(test_audio_action_t));
    if (s_action_queue == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto err;
    }
    if (s_worker_events == NULL) {
        s_worker_events = xEventGroupCreateStatic(&s_worker_event_storage);
    }

    ret = test_audio_i2s_driver_init();
    if (ret != ESP_OK) {
        goto err;
    }

    ret = test_audio_codec_init();
    if (ret != ESP_OK) {
        goto err;
    }

    now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_audio_lock);
    s_record_size = 0;
    s_record_peak = 0;
    s_record_nonzero_samples = 0;
    s_last_error = ESP_OK;
    s_initialized = true;
    s_running = false;
    s_stopping = false;
    s_state = TEST_AUDIO_STATE_IDLE;
    s_generation++;
    s_module = (test_module_meta_t) {
        .state = TEST_MODULE_STATE_READY,
        .last_error = ESP_OK,
        .generation = s_generation,
        .updated_at_us = now_us,
    };
    portEXIT_CRITICAL(&s_audio_lock);
    ESP_LOGI(TAG, "Audio initialized (sample_rate=%d, volume=%d%%, mclk_multiple=%d, record buffer lazy=%d bytes)",
             TEST_AUDIO_SAMPLE_RATE, TEST_AUDIO_VOICE_VOLUME, TEST_AUDIO_MCLK_MULTIPLE,
             TEST_AUDIO_RECORD_BUF_SIZE);
    return ESP_OK;

err:
    test_audio_release_codec();
    test_audio_release_i2s();
    if (s_action_queue != NULL) {
        vQueueDelete(s_action_queue);
        s_action_queue = NULL;
    }
    test_audio_latch_error(ret);
    return ret;
}

esp_err_t test_audio_init(void)
{
    ESP_RETURN_ON_ERROR(test_audio_lifecycle_enter(), TAG,
                        "enter audio lifecycle failed");
    esp_err_t ret = test_audio_init_locked();
    test_audio_lifecycle_exit();
    return ret;
}

static esp_err_t test_audio_run_locked(void)
{
    portENTER_CRITICAL(&s_audio_lock);
    bool initialized = s_initialized;
    bool running = s_running;
    bool stopping = s_stopping;
    portEXIT_CRITICAL(&s_audio_lock);
    if (!initialized || stopping) {
        ESP_LOGE(TAG, "Audio not initialized, call test_audio_init first");
        return ESP_ERR_INVALID_STATE;
    }

    if (running) {
        ESP_LOGI(TAG, "Audio test already running");
        return ESP_OK;
    }

    xEventGroupClearBits(s_worker_events, TEST_AUDIO_WORKER_EXITED_BIT);
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_audio_lock);
    s_stopping = false;
    s_running = true;
    s_module.state = TEST_MODULE_STATE_RUNNING;
    s_module.last_error = ESP_OK;
    s_module.updated_at_us = now_us;
    portEXIT_CRITICAL(&s_audio_lock);
    BaseType_t task_created = xTaskCreatePinnedToCore(test_audio_task,
                                                      "audio_test",
                                                      TEST_AUDIO_TASK_STACK_SIZE,
                                                      NULL,
                                                      TEST_AUDIO_TASK_PRIORITY,
                                                      &s_audio_task_handle,
                                                      TEST_AUDIO_TASK_CORE_ID);
    if (task_created != pdTRUE) {
        portENTER_CRITICAL(&s_audio_lock);
        s_running = false;
        s_audio_task_handle = NULL;
        portEXIT_CRITICAL(&s_audio_lock);
        test_audio_latch_error(ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Audio test worker started");
    return ESP_OK;
}

esp_err_t test_audio_run(void)
{
    ESP_RETURN_ON_ERROR(test_audio_lifecycle_enter(), TAG,
                        "enter audio lifecycle failed");
    esp_err_t ret = test_audio_run_locked();
    test_audio_lifecycle_exit();
    return ret;
}

esp_err_t test_audio_start_record(void)
{
    return test_audio_request_action(TEST_AUDIO_ACTION_RECORD);
}

esp_err_t test_audio_start_playback(void)
{
    return test_audio_request_action(TEST_AUDIO_ACTION_PLAYBACK);
}

esp_err_t test_audio_start_music(void)
{
    return test_audio_request_action(TEST_AUDIO_ACTION_MUSIC);
}

test_audio_state_t test_audio_get_state(void)
{
    test_audio_state_t state = TEST_AUDIO_STATE_UNINITIALIZED;

    portENTER_CRITICAL(&s_audio_lock);
    state = s_state;
    portEXIT_CRITICAL(&s_audio_lock);
    return state;
}

esp_err_t test_audio_get_status(test_audio_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != NULL, ESP_ERR_INVALID_ARG, TAG, "out_status is NULL");

    portENTER_CRITICAL(&s_audio_lock);
    *out_status = (test_audio_status_t) {
        .module = s_module,
        .initialized = s_initialized,
        .running = s_running,
        .state = s_state,
        .last_record_size = s_record_size,
        .last_record_peak = s_record_peak,
        .last_record_nonzero_count = s_record_nonzero_samples,
        .last_error = s_last_error,
    };
    portEXIT_CRITICAL(&s_audio_lock);
    return ESP_OK;
}

size_t test_audio_get_last_record_size(void)
{
    size_t value;
    portENTER_CRITICAL(&s_audio_lock);
    value = s_record_size;
    portEXIT_CRITICAL(&s_audio_lock);
    return value;
}

esp_err_t test_audio_get_last_error(void)
{
    esp_err_t value;
    portENTER_CRITICAL(&s_audio_lock);
    value = s_last_error;
    portEXIT_CRITICAL(&s_audio_lock);
    return value;
}

uint16_t test_audio_get_last_record_peak(void)
{
    uint16_t value;
    portENTER_CRITICAL(&s_audio_lock);
    value = s_record_peak;
    portEXIT_CRITICAL(&s_audio_lock);
    return value;
}

size_t test_audio_get_last_record_nonzero_count(void)
{
    size_t value;
    portENTER_CRITICAL(&s_audio_lock);
    value = s_record_nonzero_samples;
    portEXIT_CRITICAL(&s_audio_lock);
    return value;
}

static esp_err_t test_audio_deinit_locked(void)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_audio_lock);
    bool worker_exists = s_audio_task_handle != NULL;
    s_stopping = true;
    s_module.state = TEST_MODULE_STATE_STOPPING;
    s_module.updated_at_us = now_us;
    portEXIT_CRITICAL(&s_audio_lock);

    if (worker_exists) {
        test_audio_action_t action = TEST_AUDIO_ACTION_STOP;
        xQueueOverwrite(s_action_queue, &action);
        EventBits_t bits = xEventGroupWaitBits(s_worker_events,
                                               TEST_AUDIO_WORKER_EXITED_BIT,
                                               pdTRUE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(TEST_AUDIO_STOP_TIMEOUT_MS));
        if ((bits & TEST_AUDIO_WORKER_EXITED_BIT) == 0) {
            ESP_LOGE(TAG, "Audio worker did not acknowledge stop within %d ms",
                     TEST_AUDIO_STOP_TIMEOUT_MS);
            test_audio_latch_error(ESP_ERR_TIMEOUT);
            return ESP_ERR_TIMEOUT;
        }
    }

    esp_err_t ret = test_audio_release_codec();
    esp_err_t i2s_ret = test_audio_release_i2s();
    if (ret == ESP_OK) {
        ret = i2s_ret;
    }
    if (ret != ESP_OK) {
        test_audio_latch_error(ret);
        return ret;
    }

    free(s_record_buffer);
    s_record_buffer = NULL;
    if (s_action_queue != NULL) {
        vQueueDelete(s_action_queue);
        s_action_queue = NULL;
    }
    now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_audio_lock);
    s_record_size = 0;
    s_record_peak = 0;
    s_record_nonzero_samples = 0;
    s_last_error = ESP_OK;
    s_initialized = false;
    s_running = false;
    s_stopping = false;
    s_state = TEST_AUDIO_STATE_UNINITIALIZED;
    s_module = (test_module_meta_t) {
        .state = TEST_MODULE_STATE_UNINITIALIZED,
        .last_error = ESP_OK,
        .generation = s_generation,
        .updated_at_us = now_us,
    };
    portEXIT_CRITICAL(&s_audio_lock);
    ESP_LOGI(TAG, "Audio deinitialized");
    return ESP_OK;
}

esp_err_t test_audio_deinit(void)
{
    ESP_RETURN_ON_ERROR(test_audio_lifecycle_enter(), TAG,
                        "enter audio lifecycle failed");
    esp_err_t ret = test_audio_deinit_locked();
    test_audio_lifecycle_exit();
    return ret;
}
