#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*board_touch_ft3168_reset_cb_t)(void *ctx);

typedef struct {
    uint8_t i2c_addr;
    uint16_t x_max;
    uint16_t y_max;
    gpio_num_t int_gpio_num;
    board_touch_ft3168_reset_cb_t reset_cb;
    void *reset_ctx;
} board_touch_ft3168_config_t;

esp_err_t board_touch_ft3168_create(i2c_master_bus_handle_t bus,
                                    const board_touch_ft3168_config_t *config,
                                    esp_lcd_touch_handle_t *ret_touch);
bool board_touch_ft3168_is_ready(void);
bool board_touch_ft3168_should_retry_init(void);
esp_err_t board_touch_ft3168_read(uint16_t *x, uint16_t *y, uint8_t *pressed, uint8_t *point_num);
esp_err_t board_touch_ft3168_deinit(void);

#ifdef __cplusplus
}
#endif
