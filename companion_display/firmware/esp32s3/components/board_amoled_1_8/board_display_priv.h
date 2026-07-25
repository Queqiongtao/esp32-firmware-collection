#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t board_display_module_init(void);
esp_err_t board_display_module_get(esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io);
esp_err_t board_display_module_set_backlight(int percent);
esp_err_t board_lcd_power_set(bool enable);
esp_err_t board_lcd_reset_pulse(void);
esp_err_t board_touch_reset_pulse(void);

#ifdef __cplusplus
}
#endif
