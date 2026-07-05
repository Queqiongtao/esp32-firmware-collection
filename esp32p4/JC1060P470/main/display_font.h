#pragma once

#include "esp_err.h"
#include "lvgl.h"

esp_err_t display_font_init(void);
const lv_font_t *display_font_get_ui_font(void);
void display_font_deinit(void);
