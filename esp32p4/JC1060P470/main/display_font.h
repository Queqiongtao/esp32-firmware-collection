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

typedef enum {
    DISPLAY_FONT_DIAG_NOT_RUN,
    DISPLAY_FONT_DIAG_OK,
    DISPLAY_FONT_DIAG_FALLBACK,
    DISPLAY_FONT_DIAG_ERROR,
} display_font_diag_state_t;

typedef struct {
    display_font_diag_state_t state;
    const char *font_file_name;
    char stage[16];
    uint16_t role_sizes[DISPLAY_FONT_ROLE_COUNT];
    uint32_t role_ready_count;
    uint32_t role_fail_count;
    char first_error[80];
} display_font_diagnostics_t;

#define DISPLAY_FONT_SAMPLE_SIZE_MIN 14
#define DISPLAY_FONT_SAMPLE_SIZE_MAX 240
#define DISPLAY_FONT_SAMPLE_SIZE_STEP 2

esp_err_t display_font_init(void);
const lv_font_t *display_font_get_ui_font(void);
const lv_font_t *display_font_get_role_font(display_font_role_t role);
const lv_font_t *display_font_get_freetype_role_font(display_font_role_t role);
const lv_font_t *display_font_get_freetype_sample_font(uint16_t size_px);
bool display_font_is_freetype_ready(void);
const display_font_diagnostics_t *display_font_get_diagnostics(void);
void display_font_deinit(void);
