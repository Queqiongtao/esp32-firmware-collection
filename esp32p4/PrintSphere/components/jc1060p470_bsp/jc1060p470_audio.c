#include "bsp/jc1060p470.h"

#include "board_i2c.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

#define BSP_AUDIO_I2S_NUM 0
#define BSP_AUDIO_MCLK GPIO_NUM_13
#define BSP_AUDIO_BCLK GPIO_NUM_12
#define BSP_AUDIO_WS GPIO_NUM_10
#define BSP_AUDIO_DOUT GPIO_NUM_9
#define BSP_AUDIO_DIN GPIO_NUM_11
#define BSP_AUDIO_PA GPIO_NUM_20
#define BSP_AUDIO_SAMPLE_RATE 16000
#define BSP_AUDIO_MCLK_MULTIPLE 384

static const char *TAG = "jc1060p470_audio";
static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static const audio_codec_ctrl_if_t *s_ctrl;
static const audio_codec_data_if_t *s_data;
static const audio_codec_gpio_if_t *s_gpio;
static const audio_codec_if_t *s_codec;
static esp_codec_dev_handle_t s_device;
static bool s_device_open;
static bool s_i2c_acquired;

void bsp_audio_codec_speaker_deinit(void)
{
    if (s_device != NULL) {
        if (s_device_open) {
            (void)esp_codec_dev_close(s_device);
        }
        esp_codec_dev_delete(s_device);
        s_device = NULL;
        s_device_open = false;
    }
    if (s_codec != NULL) {
        (void)audio_codec_delete_codec_if(s_codec);
        s_codec = NULL;
    }
    if (s_gpio != NULL) {
        (void)audio_codec_delete_gpio_if(s_gpio);
        s_gpio = NULL;
    }
    if (s_data != NULL) {
        (void)audio_codec_delete_data_if(s_data);
        s_data = NULL;
    }
    if (s_ctrl != NULL) {
        (void)audio_codec_delete_ctrl_if(s_ctrl);
        s_ctrl = NULL;
    }
    if (s_tx != NULL) {
        (void)i2s_channel_disable(s_tx);
        (void)i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    if (s_rx != NULL) {
        (void)i2s_channel_disable(s_rx);
        (void)i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    if (s_i2c_acquired) {
        (void)board_i2c_deinit();
        s_i2c_acquired = false;
    }
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    if (s_device != NULL) {
        return s_device;
    }

    if (board_i2c_init() != ESP_OK) {
        return NULL;
    }
    s_i2c_acquired = true;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BSP_AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &s_tx, &s_rx) != ESP_OK) {
        goto fail;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_AUDIO_MCLK,
            .bclk = BSP_AUDIO_BCLK,
            .ws = BSP_AUDIO_WS,
            .dout = BSP_AUDIO_DOUT,
            .din = BSP_AUDIO_DIN,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = BSP_AUDIO_MCLK_MULTIPLE;
    if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK ||
        i2s_channel_init_std_mode(s_rx, &std_cfg) != ESP_OK ||
        i2s_channel_enable(s_tx) != ESP_OK ||
        i2s_channel_enable(s_rx) != ESP_OK) {
        ESP_LOGE(TAG, "I2S initialization failed");
        goto fail;
    }

    i2c_master_bus_handle_t bus = NULL;
    i2c_port_num_t port = I2C_NUM_1;
    if (board_i2c_get_bus(&bus, &port) != ESP_OK) {
        goto fail;
    }
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = port,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    s_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BSP_AUDIO_I2S_NUM,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    s_data = audio_codec_new_i2s_data(&i2s_cfg);
    s_gpio = audio_codec_new_gpio();
    if (s_ctrl == NULL || s_data == NULL || s_gpio == NULL) {
        goto fail;
    }

    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if = s_ctrl,
        .gpio_if = s_gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .master_mode = false,
        .use_mclk = true,
        .pa_pin = BSP_AUDIO_PA,
        .pa_reverted = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .mclk_div = BSP_AUDIO_MCLK_MULTIPLE,
    };
    s_codec = es8311_codec_new(&codec_cfg);
    if (s_codec == NULL) {
        goto fail;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_codec,
        .data_if = s_data,
    };
    s_device = esp_codec_dev_new(&dev_cfg);
    if (s_device == NULL) {
        goto fail;
    }

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = BSP_AUDIO_SAMPLE_RATE,
        .mclk_multiple = BSP_AUDIO_MCLK_MULTIPLE,
    };
    if (esp_codec_dev_open(s_device, &sample_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 initialization failed");
        goto fail;
    }
    s_device_open = true;
    if (esp_codec_dev_set_out_vol(s_device, 60) != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 volume initialization failed");
        goto fail;
    }
    ESP_LOGI(TAG, "ES8311 speaker ready at %d Hz", BSP_AUDIO_SAMPLE_RATE);
    return s_device;

fail:
    bsp_audio_codec_speaker_deinit();
    return NULL;
}
