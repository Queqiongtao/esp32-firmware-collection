#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_LCD_TOUCH_INT GPIO_NUM_21

typedef enum {
    BSP_DISPLAY_ROTATE_0 = 0,
    BSP_DISPLAY_ROTATE_90,
    BSP_DISPLAY_ROTATE_180,
    BSP_DISPLAY_ROTATE_270,
} bsp_display_rotation_t;

typedef struct {
    esp_lv_adapter_config_t lv_adapter_cfg;
    esp_lv_adapter_rotation_t rotation;
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode;
} bsp_display_cfg_t;

lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *config);
esp_err_t bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);
esp_err_t bsp_display_brightness_set(int brightness_percent);
esp_err_t bsp_display_rotation_set(bsp_display_rotation_t rotation);

esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);
void bsp_audio_codec_speaker_deinit(void);

esp_err_t bsp_wifi_hosted_init(void);
bool bsp_touch_is_active(void);

#ifdef __cplusplus
}
#endif
