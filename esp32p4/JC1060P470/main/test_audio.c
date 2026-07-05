/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "test_audio.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "display_touch.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "test_config.h"

static const char *TAG = "test_audio";

#define TEST_AUDIO_ES8311_SYSTEM_REG14         0x14
#define TEST_AUDIO_ES8311_ADC_REG15            0x15
#define TEST_AUDIO_ES8311_ADC_REG16            0x16
#define TEST_AUDIO_ES8311_ADC_REG17            0x17
#define TEST_AUDIO_ES8311_GPIO_REG44           0x44

#if CONFIG_EXAMPLE_AUDIO_MODE_MUSIC
extern const uint8_t canon_pcm_start[] asm("_binary_canon_pcm_start");
extern const uint8_t canon_pcm_end[]   asm("_binary_canon_pcm_end");
#endif

typedef enum {
    TEST_AUDIO_ACTION_NONE = 0,
    TEST_AUDIO_ACTION_RECORD,
    TEST_AUDIO_ACTION_PLAYBACK,
} test_audio_action_t;

static i2s_chan_handle_t s_tx_handle = NULL;
static i2s_chan_handle_t s_rx_handle = NULL;
static esp_codec_dev_handle_t s_codec_dev = NULL;
static TaskHandle_t s_audio_task_handle = NULL;
static portMUX_TYPE s_audio_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *s_record_buffer = NULL;
static size_t s_record_size = 0;
static uint16_t s_record_peak = 0;
static size_t s_record_nonzero_samples = 0;
static test_audio_state_t s_state = TEST_AUDIO_STATE_UNINITIALIZED;
static test_audio_action_t s_pending_action = TEST_AUDIO_ACTION_NONE;
static esp_err_t s_last_error = ESP_OK;
static bool s_initialized = false;
static bool s_running = false;
static bool s_stopping = false;

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

static void test_audio_update_record_stats(const uint8_t *data, size_t bytes)
{
    const int16_t *samples = (const int16_t *)data;
    size_t sample_count = bytes / sizeof(int16_t);

    for (size_t i = 0; i < sample_count; ++i) {
        int32_t sample = samples[i];
        uint32_t amplitude = (sample == INT16_MIN) ? (uint32_t)INT16_MAX + 1U : (uint32_t)(sample < 0 ? -sample : sample);
        if (amplitude > s_record_peak) {
            s_record_peak = (uint16_t)(amplitude > UINT16_MAX ? UINT16_MAX : amplitude);
        }
        if (sample != 0) {
            s_record_nonzero_samples++;
        }
    }
}

static const char *test_audio_describe_input_level(void)
{
    if (s_record_peak == 0 || s_record_nonzero_samples == 0) {
        return "silent";
    }
    if (s_record_peak < 256) {
        return "weak";
    }
    return "active";
}

static void test_audio_set_state(test_audio_state_t state)
{
    portENTER_CRITICAL(&s_audio_lock);
    s_state = state;
    portEXIT_CRITICAL(&s_audio_lock);
}

static test_audio_action_t test_audio_take_pending_action(void)
{
    test_audio_action_t action = TEST_AUDIO_ACTION_NONE;

    portENTER_CRITICAL(&s_audio_lock);
    action = s_pending_action;
    s_pending_action = TEST_AUDIO_ACTION_NONE;
    portEXIT_CRITICAL(&s_audio_lock);
    return action;
}

static void test_audio_set_pending_action(test_audio_action_t action)
{
    portENTER_CRITICAL(&s_audio_lock);
    s_pending_action = action;
    portEXIT_CRITICAL(&s_audio_lock);
}

static void test_audio_latch_error(esp_err_t err)
{
    s_last_error = err;
    test_audio_set_state(TEST_AUDIO_STATE_ERROR);
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
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "enable I2S rx failed");
    return ESP_OK;
}

static esp_err_t test_audio_codec_init(void)
{
    i2c_master_bus_handle_t i2c_bus_handle = NULL;
    int i2c_port = 0;
    ESP_RETURN_ON_ERROR(display_touch_get_i2c_bus(&i2c_bus_handle, &i2c_port),
                        TAG, "get shared I2C bus handle failed");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = i2c_port,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus_handle,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "create I2C control interface failed");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = TEST_AUDIO_I2S_NUM,
        .rx_handle = s_rx_handle,
        .tx_handle = s_tx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "create I2S data interface failed");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_FAIL, TAG, "create GPIO interface failed");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
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
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(es8311_if, ESP_FAIL, TAG, "create ES8311 codec interface failed");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = es8311_if,
        .data_if = data_if,
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
#if !CONFIG_EXAMPLE_AUDIO_MODE_MUSIC
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_codec_dev, TEST_AUDIO_MIC_GAIN), TAG, "set input gain failed");
#endif
    test_audio_dump_input_regs("codec_init");
    return ESP_OK;
}

static esp_err_t test_audio_record_once(void)
{
    ESP_RETURN_ON_FALSE(s_record_buffer, ESP_ERR_INVALID_STATE, TAG, "record buffer not allocated");

    test_audio_set_state(TEST_AUDIO_STATE_RECORDING);
    s_last_error = ESP_OK;
    memset(s_record_buffer, 0, TEST_AUDIO_RECORD_BUF_SIZE);
    s_record_size = 0;
    s_record_peak = 0;
    s_record_nonzero_samples = 0;

    ESP_LOGI(TAG, "Start recording: %d seconds, target=%d bytes",
             TEST_AUDIO_RECORD_SECONDS, TEST_AUDIO_RECORD_BUF_SIZE);
    test_audio_dump_input_regs("record_start");

    while (!s_stopping && s_record_size < TEST_AUDIO_RECORD_BUF_SIZE) {
        size_t bytes_to_read = TEST_AUDIO_RECV_BUF_SIZE;
        size_t remaining = TEST_AUDIO_RECORD_BUF_SIZE - s_record_size;
        if (bytes_to_read > remaining) {
            bytes_to_read = remaining;
        }

        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_rx_handle,
                                         s_record_buffer + s_record_size,
                                         bytes_to_read,
                                         &bytes_read,
                                         pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            if (s_stopping && ret == ESP_ERR_TIMEOUT) {
                break;
            }
            ESP_LOGE(TAG, "i2s read failed during record: %s", esp_err_to_name(ret));
            test_audio_latch_error(ret);
            return ret;
        }
        if (bytes_read == 0) {
            continue;
        }
        test_audio_update_record_stats(s_record_buffer + s_record_size, bytes_read);
        s_record_size += bytes_read;
    }

    if (s_stopping) {
        return ESP_OK;
    }

    test_audio_set_state(TEST_AUDIO_STATE_RECORDED);
    ESP_LOGI(TAG, "Recording finished, valid=%d bytes, peak=%u, nonzero_samples=%u, input=%s",
             (int)s_record_size, (unsigned int)s_record_peak, (unsigned int)s_record_nonzero_samples,
             test_audio_describe_input_level());
    return ESP_OK;
}

static esp_err_t test_audio_playback_once(void)
{
    ESP_RETURN_ON_FALSE(s_record_buffer && s_record_size > 0, ESP_ERR_INVALID_STATE, TAG, "no recorded audio");

    test_audio_set_state(TEST_AUDIO_STATE_PLAYING);
    s_last_error = ESP_OK;
    ESP_LOGI(TAG, "Start playback: valid=%d bytes", (int)s_record_size);

    size_t offset = 0;
    while (!s_stopping && offset < s_record_size) {
        size_t bytes_write = 0;
        size_t bytes_to_write = s_record_size - offset;
        if (bytes_to_write > TEST_AUDIO_RECV_BUF_SIZE) {
            bytes_to_write = TEST_AUDIO_RECV_BUF_SIZE;
        }

        esp_err_t ret = i2s_channel_write(s_tx_handle,
                                          s_record_buffer + offset,
                                          bytes_to_write,
                                          &bytes_write,
                                          pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            if (s_stopping && ret == ESP_ERR_TIMEOUT) {
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

    if (s_stopping) {
        return ESP_OK;
    }

    test_audio_set_state(TEST_AUDIO_STATE_RECORDED);
    ESP_LOGI(TAG, "Playback finished");
    return ESP_OK;
}

#if CONFIG_EXAMPLE_AUDIO_MODE_MUSIC
static esp_err_t test_audio_play_music(void)
{
    ESP_LOGI(TAG, "Playing canon.pcm (%d bytes) in loop", (int)(canon_pcm_end - canon_pcm_start));

    uint8_t *data_ptr = (uint8_t *)canon_pcm_start;
    size_t bytes_write = 0;

    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx_handle), TAG, "disable tx failed");
    ESP_RETURN_ON_ERROR(i2s_channel_preload_data(s_tx_handle, data_ptr,
                         canon_pcm_end - data_ptr, &bytes_write), TAG, "preload data failed");
    data_ptr += bytes_write;
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "enable tx failed");

    while (!s_stopping) {
        esp_err_t ret = i2s_channel_write(s_tx_handle, data_ptr,
                                          canon_pcm_end - data_ptr, &bytes_write, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            if (s_stopping && ret == ESP_ERR_TIMEOUT) {
                break;
            }
            ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(ret));
            return ret;
        }
        if (bytes_write > 0) {
            ESP_LOGI(TAG, "i2s music played, %d bytes written", (int)bytes_write);
        }
        data_ptr = (uint8_t *)canon_pcm_start;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return ESP_OK;
}
#elif CONFIG_EXAMPLE_AUDIO_MODE_ECHO
static esp_err_t test_audio_echo(void)
{
    ESP_LOGI(TAG, "Echo mode: microphone -> speaker");

    uint8_t *mic_data = malloc(TEST_AUDIO_RECV_BUF_SIZE);
    ESP_RETURN_ON_FALSE(mic_data, ESP_ERR_NO_MEM, TAG, "no memory for read buffer");

    while (!s_stopping) {
        size_t bytes_read = 0;
        size_t bytes_write = 0;
        memset(mic_data, 0, TEST_AUDIO_RECV_BUF_SIZE);
        esp_err_t ret = i2s_channel_read(s_rx_handle, mic_data,
                                         TEST_AUDIO_RECV_BUF_SIZE, &bytes_read, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            if (s_stopping && ret == ESP_ERR_TIMEOUT) {
                break;
            }
            ESP_LOGE(TAG, "i2s read failed: %s", esp_err_to_name(ret));
            free(mic_data);
            return ret;
        }
        if (bytes_read == 0) {
            continue;
        }
        ret = i2s_channel_write(s_tx_handle, mic_data,
                                bytes_read, &bytes_write, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            if (s_stopping && ret == ESP_ERR_TIMEOUT) {
                break;
            }
            ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(ret));
            free(mic_data);
            return ret;
        }
        if (bytes_read != bytes_write) {
            ESP_LOGW(TAG, "%d bytes read but only %d bytes written", (int)bytes_read, (int)bytes_write);
        }
    }
    free(mic_data);
    return ESP_OK;
}
#else
static esp_err_t test_audio_record_playback_service(void)
{
    test_audio_set_state(TEST_AUDIO_STATE_IDLE);
    s_last_error = ESP_OK;
    ESP_LOGI(TAG, "Record/playback mode ready");

    while (!s_stopping) {
        test_audio_action_t action = test_audio_take_pending_action();
        esp_err_t ret = ESP_OK;

        switch (action) {
        case TEST_AUDIO_ACTION_RECORD:
            ret = test_audio_record_once();
            break;
        case TEST_AUDIO_ACTION_PLAYBACK:
            ret = test_audio_playback_once();
            break;
        case TEST_AUDIO_ACTION_NONE:
        default:
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (ret != ESP_OK && !s_stopping) {
            return ret;
        }

        if (!s_stopping && test_audio_get_state() == TEST_AUDIO_STATE_RECORDING) {
            test_audio_set_state(TEST_AUDIO_STATE_IDLE);
        } else if (!s_stopping && test_audio_get_state() == TEST_AUDIO_STATE_PLAYING) {
            test_audio_set_state(TEST_AUDIO_STATE_RECORDED);
        }
    }

    return ESP_OK;
}
#endif

static esp_err_t test_audio_worker_run(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Audio not initialized, call test_audio_init first");
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_EXAMPLE_AUDIO_MODE_MUSIC
    return test_audio_play_music();
#elif CONFIG_EXAMPLE_AUDIO_MODE_ECHO
    return test_audio_echo();
#else
    return test_audio_record_playback_service();
#endif
}

static void test_audio_task(void *arg)
{
    (void)arg;

    esp_err_t ret = test_audio_worker_run();
    if (ret != ESP_OK && !s_stopping) {
        ESP_LOGE(TAG, "Audio worker exited with error: %s", esp_err_to_name(ret));
        test_audio_latch_error(ret);
    }

    s_running = false;
    s_audio_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t test_audio_init(void)
{
    ESP_LOGI(TAG, "Initializing audio (I2S + ES8311 codec)");

    if (s_initialized) {
        return ESP_OK;
    }

    s_record_buffer = malloc(TEST_AUDIO_RECORD_BUF_SIZE);
    ESP_RETURN_ON_FALSE(s_record_buffer, ESP_ERR_NO_MEM, TAG, "allocate record buffer failed");

    esp_err_t ret = test_audio_i2s_driver_init();
    if (ret != ESP_OK) {
        free(s_record_buffer);
        s_record_buffer = NULL;
        return ret;
    }

    ret = test_audio_codec_init();
    if (ret != ESP_OK) {
        free(s_record_buffer);
        s_record_buffer = NULL;
        return ret;
    }

    s_record_size = 0;
    s_record_peak = 0;
    s_record_nonzero_samples = 0;
    s_last_error = ESP_OK;
    s_initialized = true;
    test_audio_set_pending_action(TEST_AUDIO_ACTION_NONE);
    test_audio_set_state(TEST_AUDIO_STATE_IDLE);
    ESP_LOGI(TAG, "Audio initialized (sample_rate=%d, volume=%d%%, mclk_multiple=%d, record_buf=%d bytes)",
             TEST_AUDIO_SAMPLE_RATE, TEST_AUDIO_VOICE_VOLUME, TEST_AUDIO_MCLK_MULTIPLE, TEST_AUDIO_RECORD_BUF_SIZE);
    return ESP_OK;
}

esp_err_t test_audio_run(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Audio not initialized, call test_audio_init first");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_running) {
        ESP_LOGI(TAG, "Audio test already running");
        return ESP_OK;
    }

    s_stopping = false;
    BaseType_t task_created = xTaskCreatePinnedToCore(test_audio_task,
                                                      "audio_test",
                                                      TEST_AUDIO_TASK_STACK_SIZE,
                                                      NULL,
                                                      TEST_AUDIO_TASK_PRIORITY,
                                                      &s_audio_task_handle,
                                                      TEST_AUDIO_TASK_CORE_ID);
    ESP_RETURN_ON_FALSE(task_created == pdTRUE, ESP_ERR_NO_MEM, TAG, "create audio worker task failed");
    s_running = true;
    ESP_LOGI(TAG, "Audio test worker started");
    return ESP_OK;
}

esp_err_t test_audio_start_record(void)
{
#if CONFIG_EXAMPLE_AUDIO_MODE_MUSIC || CONFIG_EXAMPLE_AUDIO_MODE_ECHO
    ESP_LOGW(TAG, "Record command ignored in current audio mode");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_initialized || !s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    test_audio_state_t state = test_audio_get_state();
    ESP_RETURN_ON_FALSE(state == TEST_AUDIO_STATE_IDLE || state == TEST_AUDIO_STATE_RECORDED || state == TEST_AUDIO_STATE_ERROR,
                        ESP_ERR_INVALID_STATE, TAG, "audio busy, current state=%d", state);

    s_last_error = ESP_OK;
    test_audio_set_pending_action(TEST_AUDIO_ACTION_RECORD);
    return ESP_OK;
#endif
}

esp_err_t test_audio_start_playback(void)
{
#if CONFIG_EXAMPLE_AUDIO_MODE_MUSIC || CONFIG_EXAMPLE_AUDIO_MODE_ECHO
    ESP_LOGW(TAG, "Playback command ignored in current audio mode");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_initialized || !s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    test_audio_state_t state = test_audio_get_state();
    ESP_RETURN_ON_FALSE(state == TEST_AUDIO_STATE_RECORDED,
                        ESP_ERR_INVALID_STATE, TAG, "audio not ready for playback, current state=%d", state);
    ESP_RETURN_ON_FALSE(s_record_size > 0, ESP_ERR_INVALID_STATE, TAG, "no recorded audio available");

    s_last_error = ESP_OK;
    test_audio_set_pending_action(TEST_AUDIO_ACTION_PLAYBACK);
    return ESP_OK;
#endif
}

test_audio_state_t test_audio_get_state(void)
{
    test_audio_state_t state = TEST_AUDIO_STATE_UNINITIALIZED;

    portENTER_CRITICAL(&s_audio_lock);
    state = s_state;
    portEXIT_CRITICAL(&s_audio_lock);
    return state;
}

size_t test_audio_get_last_record_size(void)
{
    return s_record_size;
}

esp_err_t test_audio_get_last_error(void)
{
    return s_last_error;
}

uint16_t test_audio_get_last_record_peak(void)
{
    return s_record_peak;
}

size_t test_audio_get_last_record_nonzero_count(void)
{
    return s_record_nonzero_samples;
}

esp_err_t test_audio_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    s_stopping = true;
    if (s_audio_task_handle != NULL) {
        for (int i = 0; i < 10 && s_audio_task_handle != NULL; ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_audio_task_handle != NULL) {
            ESP_LOGW(TAG, "Audio worker task did not exit in time, deleting it");
            vTaskDelete(s_audio_task_handle);
            s_audio_task_handle = NULL;
        }
    }
    s_running = false;

    if (s_codec_dev) {
        esp_codec_dev_close(s_codec_dev);
        esp_codec_dev_delete(s_codec_dev);
        s_codec_dev = NULL;
    }

    if (s_tx_handle) {
        i2s_channel_disable(s_tx_handle);
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
    }
    if (s_rx_handle) {
        i2s_channel_disable(s_rx_handle);
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
    }

    free(s_record_buffer);
    s_record_buffer = NULL;
    s_record_size = 0;
    s_record_peak = 0;
    s_record_nonzero_samples = 0;
    s_last_error = ESP_OK;
    s_initialized = false;
    s_stopping = false;
    test_audio_set_pending_action(TEST_AUDIO_ACTION_NONE);
    test_audio_set_state(TEST_AUDIO_STATE_UNINITIALIZED);
    ESP_LOGI(TAG, "Audio deinitialized");
    return ESP_OK;
}
