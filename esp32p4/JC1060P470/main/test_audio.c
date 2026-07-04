/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "test_audio.h"

#include "test_config.h"
#include "display_touch.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "test_audio";

#if CONFIG_EXAMPLE_AUDIO_MODE_MUSIC
extern const uint8_t canon_pcm_start[] asm("_binary_canon_pcm_start");
extern const uint8_t canon_pcm_end[]   asm("_binary_canon_pcm_end");
#endif

static i2s_chan_handle_t s_tx_handle = NULL;
static i2s_chan_handle_t s_rx_handle = NULL;
static esp_codec_dev_handle_t s_codec_dev = NULL;
static TaskHandle_t s_audio_task_handle = NULL;
static bool s_initialized = false;
static bool s_running = false;
static bool s_stopping = false;

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
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = TEST_AUDIO_SAMPLE_RATE,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_codec_dev, &sample_cfg), TAG, "open codec device failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_codec_dev, TEST_AUDIO_VOICE_VOLUME), TAG, "set output volume failed");
#if CONFIG_EXAMPLE_AUDIO_MODE_ECHO
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_codec_dev, TEST_AUDIO_MIC_GAIN), TAG, "set input gain failed");
#endif
    return ESP_OK;
}

esp_err_t test_audio_init(void)
{
    ESP_LOGI(TAG, "Initializing audio (I2S + ES8311 codec)");

    ESP_RETURN_ON_ERROR(test_audio_i2s_driver_init(), TAG, "I2S driver init failed");
    ESP_RETURN_ON_ERROR(test_audio_codec_init(), TAG, "codec init failed");

    s_initialized = true;
    ESP_LOGI(TAG, "Audio initialized (sample_rate=%d, volume=%d%%)",
             TEST_AUDIO_SAMPLE_RATE, TEST_AUDIO_VOICE_VOLUME);
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
#else
static esp_err_t test_audio_echo(void)
{
    ESP_LOGI(TAG, "Echo mode: microphone -> speaker");

    int *mic_data = malloc(TEST_AUDIO_RECV_BUF_SIZE);
    ESP_RETURN_ON_FALSE(mic_data, ESP_ERR_NO_MEM, TAG, "no memory for read buffer");

    size_t bytes_read = 0;
    size_t bytes_write = 0;

    while (!s_stopping) {
        memset(mic_data, 0, TEST_AUDIO_RECV_BUF_SIZE);
        esp_err_t ret = i2s_channel_read(s_rx_handle, mic_data,
                                         TEST_AUDIO_RECV_BUF_SIZE, &bytes_read, 1000);
        if (ret != ESP_OK) {
            if (s_stopping && ret == ESP_ERR_TIMEOUT) {
                break;
            }
            ESP_LOGE(TAG, "i2s read failed: %s", esp_err_to_name(ret));
            free(mic_data);
            return ret;
        }
        ret = i2s_channel_write(s_tx_handle, mic_data,
                                TEST_AUDIO_RECV_BUF_SIZE, &bytes_write, 1000);
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
#endif

static esp_err_t test_audio_worker_run(void);

static void test_audio_task(void *arg)
{
    (void)arg;

    esp_err_t ret = test_audio_worker_run();
    if (ret != ESP_OK && !s_stopping) {
        ESP_LOGE(TAG, "Audio worker exited with error: %s", esp_err_to_name(ret));
    }

    s_running = false;
    s_audio_task_handle = NULL;
    vTaskDelete(NULL);
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

static esp_err_t test_audio_worker_run(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Audio not initialized, call test_audio_init first");
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_EXAMPLE_AUDIO_MODE_MUSIC
    return test_audio_play_music();
#else
    return test_audio_echo();
#endif
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

    s_initialized = false;
    s_stopping = false;
    ESP_LOGI(TAG, "Audio deinitialized");
    return ESP_OK;
}
