#pragma once

#include "esp_err.h"
#include "lvgl.h"

typedef enum {
    DISPLAY_FONT_ROLE_SMALL,
    DISPLAY_FONT_ROLE_BODY,
    DISPLAY_FONT_ROLE_CARD_TITLE,
    DISPLAY_FONT_ROLE_TITLE,
    DISPLAY_FONT_ROLE_COUNT,
} display_font_role_t;

esp_err_t display_font_init(void);
const lv_font_t *display_font_get_ui_font(void);
const lv_font_t *display_font_get_role_font(display_font_role_t role);
void display_font_deinit(void);
