#include "clock_theme.h"

#include <stdio.h>

#include "lvgl.h"

extern "C" const lv_font_t lv_font_clock_96;

static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_seconds_label = NULL;
static lv_obj_t *s_date_label = NULL;

static lv_obj_t *minimal_build(lv_obj_t *parent)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(container, 16, 0);
    lv_obj_center(container);

    s_time_label = lv_label_create(container);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_time_label, &lv_font_clock_96, 0);
    lv_label_set_text(s_time_label, "--:--");

    s_seconds_label = lv_label_create(container);
    lv_obj_set_style_text_color(s_seconds_label, lv_color_hex(0x8A8F98), 0);
    lv_obj_set_style_text_font(s_seconds_label, &lv_font_montserrat_40, 0);
    lv_label_set_text(s_seconds_label, "--");

    s_date_label = lv_label_create(container);
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(0x8A8F98), 0);
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_letter_space(s_date_label, 1, 0);
    lv_label_set_text(s_date_label, "---  --:--");

    return container;
}

static bool minimal_update(const struct tm *timeinfo)
{
    static const char *const kWeekdayAbbr[7] = {
        "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

    char time_text[8] = {};
    char sec_text[4] = {};
    char date_text[16] = {};

    snprintf(time_text, sizeof(time_text), "%02d:%02d",
             timeinfo->tm_hour, timeinfo->tm_min);
    snprintf(sec_text, sizeof(sec_text), "%02d", timeinfo->tm_sec);
    snprintf(date_text, sizeof(date_text), "%s  %02d-%02d",
             kWeekdayAbbr[timeinfo->tm_wday % 7], timeinfo->tm_mon + 1, timeinfo->tm_mday);

    lv_label_set_text(s_time_label, time_text);
    lv_label_set_text(s_seconds_label, sec_text);
    lv_label_set_text(s_date_label, date_text);
    return true;
}

const clock_theme_t g_clock_theme_minimal = {
    .name = "minimal",
    .build = minimal_build,
    .update = minimal_update,
};
