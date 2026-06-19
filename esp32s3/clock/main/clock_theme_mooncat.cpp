#include "clock_theme.h"

#include <stddef.h>
#include <stdio.h>

#include "lvgl.h"
#include "mooncat_frames.h"

extern "C" const lv_font_t lv_font_clock_96;

namespace {

constexpr int32_t THEME_SIZE = 466;
constexpr uint32_t MOONCAT_FRAME_FPS = 12;
constexpr uint32_t MOONCAT_FRAME_INTERVAL_MS = 1000 / MOONCAT_FRAME_FPS;

static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_date_label = NULL;
static lv_obj_t *s_seconds_label = NULL;
static lv_obj_t *s_cat_image = NULL;
static uint32_t s_last_frame_tick_ms = 0;
static size_t s_frame_index = 0;

static void mooncat_apply_frame(size_t index)
{
    if (s_cat_image == NULL || g_mooncat_frame_count == 0) {
        return;
    }

    s_frame_index = index % g_mooncat_frame_count;
    lv_image_set_src(s_cat_image, g_mooncat_frames[s_frame_index]);
}

} // namespace

static lv_obj_t *mooncat_build(lv_obj_t *parent)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(container, THEME_SIZE, THEME_SIZE);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_center(container);

    s_time_label = lv_label_create(container);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_time_label, &lv_font_clock_96, 0);
    lv_label_set_text(s_time_label, "--:--");
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 60);

    s_date_label = lv_label_create(container);
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(0xA7ADB7), 0);
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(s_date_label, 1, 0);
    lv_label_set_text(s_date_label, "---  --/--");
    lv_obj_align(s_date_label, LV_ALIGN_TOP_MID, 0, 164);

    lv_obj_t *card = lv_obj_create(container);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 348, 236);
    lv_obj_set_style_radius(card, 34, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -26);

    s_seconds_label = lv_label_create(card);
    lv_obj_set_style_text_color(s_seconds_label, lv_color_hex(0x8E97A3), 0);
    lv_obj_set_style_text_font(s_seconds_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_seconds_label, "--");
    lv_obj_align(s_seconds_label, LV_ALIGN_TOP_MID, 0, 18);

    s_cat_image = lv_image_create(card);
    lv_obj_align(s_cat_image, LV_ALIGN_BOTTOM_MID, 0, -10);
    mooncat_apply_frame(0);

    return container;
}

static void mooncat_update(const struct tm *timeinfo)
{
    static const char *const kWeekdayAbbr[7] = {
        "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

    char time_text[8] = {};
    char date_text[16] = {};
    char sec_text[4] = {};

    snprintf(time_text, sizeof(time_text), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
    snprintf(date_text, sizeof(date_text), "%s  %02d/%02d",
             kWeekdayAbbr[timeinfo->tm_wday % 7], timeinfo->tm_mon + 1, timeinfo->tm_mday);
    snprintf(sec_text, sizeof(sec_text), "%02d", timeinfo->tm_sec);

    lv_label_set_text(s_time_label, time_text);
    lv_label_set_text(s_date_label, date_text);
    lv_label_set_text(s_seconds_label, sec_text);
}

static void mooncat_reset(void)
{
    s_last_frame_tick_ms = 0;
    mooncat_apply_frame(0);
}

static bool mooncat_tick(uint32_t now_ms)
{
    if (g_mooncat_frame_count <= 1 || s_cat_image == NULL) {
        return false;
    }

    if (s_last_frame_tick_ms == 0) {
        s_last_frame_tick_ms = now_ms;
        return false;
    }

    if ((uint32_t)(now_ms - s_last_frame_tick_ms) < MOONCAT_FRAME_INTERVAL_MS) {
        return false;
    }

    s_last_frame_tick_ms = now_ms;
    mooncat_apply_frame((s_frame_index + 1) % g_mooncat_frame_count);
    return true;
}

const clock_theme_t g_clock_theme_mooncat = {
    .name = "mooncat",
    .build = mooncat_build,
    .update = mooncat_update,
    .reset = mooncat_reset,
    .tick = mooncat_tick,
};
