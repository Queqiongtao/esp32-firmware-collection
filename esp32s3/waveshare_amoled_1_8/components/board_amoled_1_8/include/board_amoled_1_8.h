#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sda_gpio;
    int scl_gpio;
    int i2c_port;
    int i2c_hz;
    int backlight_gpio;
    int backlight_ledc_channel;
    int backlight_ledc_timer;
    int backlight_ledc_mode;
    int backlight_ledc_freq_hz;
    int backlight_invert;
} board_amoled_1_8_config_t;

const board_amoled_1_8_config_t *board_amoled_1_8_get_config(void);
esp_err_t board_i2c_init(void);
esp_err_t board_display_init(void);
esp_err_t board_backlight_set(int percent);

#ifdef __cplusplus
}
#endif
