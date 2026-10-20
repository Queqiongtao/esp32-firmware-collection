#include "clock_theme.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>

#include "lvgl.h"
#include "mooncat_frames.h"

extern "C" const lv_font_t lv_font_clock_96;

namespace {

constexpr int32_t THEME_SIZE = 466;
constexpr int32_t MOONCAT_CARD_WIDTH = 348;
constexpr int32_t MOONCAT_CARD_HEIGHT = 236;
constexpr int32_t MOONCAT_IMAGE_WIDTH = 180;
constexpr int32_t MOONCAT_IMAGE_HEIGHT = 193;
constexpr int32_t MOONCAT_IMAGE_CENTER_X = THEME_SIZE / 2;
constexpr int32_t MOONCAT_IMAGE_CENTER_Y = THEME_SIZE - 26 - 10 - MOONCAT_IMAGE_HEIGHT / 2;

static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_date_label = NULL;
static lv_obj_t *s_seconds_label = NULL;
static lv_obj_t *s_cat_image = NULL;
static uint32_t s_last_frame_tick_ms = 0;
static size_t s_frame_index = 0;
static float s_rotation_deg = 0.0f;

static int32_t round_to_i32(float value)
{
    return (int32_t)lroundf(value);
}

static void mooncat_apply_frame(size_t index)
{
    if (s_cat_image == NULL || g_mooncat_frame_count == 0) {
        return;
    }

    s_frame_index = index % g_mooncat_frame_count;
    lv_image_set_src(s_cat_image, g_mooncat_frames[s_frame_index]);
}

static void mooncat_position_overlay(void)
{
    if (s_cat_image == NULL) {
        return;
    }

    const float radians = s_rotation_deg * (float)M_PI / 180.0f;
    const float cos_a = cosf(radians);
    const float sin_a = sinf(radians);
    const float dx = (float)(MOONCAT_IMAGE_CENTER_X - THEME_SIZE / 2);
    const float dy = (float)(MOONCAT_IMAGE_CENTER_Y - THEME_SIZE / 2);
    const int32_t center_x = THEME_SIZE / 2 + round_to_i32(cos_a * dx - sin_a * dy);
    const int32_t center_y = THEME_SIZE / 2 + round_to_i32(sin_a * dx + cos_a * dy);

    lv_image_set_rotation(s_cat_image, (int32_t)(s_rotation_deg * 10.0f));
    lv_obj_set_pos(s_cat_image,
                   center_x - MOONCAT_IMAGE_WIDTH / 2,
                   center_y - MOONCAT_IMAGE_HEIGHT / 2);
}

static void mooncat_destroy_overlay(void)
{
    if (s_cat_image != NULL) {
        lv_obj_del(s_cat_image);
        s_cat_image = NULL;
    }
}

} // namespace

static lv_obj_t *mooncat_build(lv_obj_t *parent)
{
    mooncat_destroy_overlay();

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
    lv_obj_set_size(card, MOONCAT_CARD_WIDTH, MOONCAT_CARD_HEIGHT);
    lv_obj_set_style_radius(card, 34, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -26);

    s_seconds_label = lv_label_create(card);
    lv_obj_set_style_text_color(s_seconds_label, lv_color_hex(0x8E97A3), 0);
    lv_obj_set_style_text_font(s_seconds_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_seconds_label, "--");
    lv_obj_align(s_seconds_label, LV_ALIGN_TOP_MID, 0, 18);

    s_cat_image = lv_image_create(parent);
    lv_obj_clear_flag(s_cat_image, LV_OBJ_FLAG_SCROLLABLE);
    lv_image_set_pivot(s_cat_image, MOONCAT_IMAGE_WIDTH / 2, MOONCAT_IMAGE_HEIGHT / 2);
    mooncat_apply_frame(0);
    mooncat_position_overlay();

    return container;
}

static bool mooncat_update(const struct tm *timeinfo)
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
    return true;
}

static void mooncat_reset(void)
{
    s_last_frame_tick_ms = 0;
    mooncat_apply_frame(0);
    mooncat_position_overlay();
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

    const uint32_t frame_duration_ms = g_mooncat_frame_durations_ms[s_frame_index];
    if ((uint32_t)(now_ms - s_last_frame_tick_ms) < frame_duration_ms) {
        return false;
    }

    s_last_frame_tick_ms += frame_duration_ms;
    mooncat_apply_frame((s_frame_index + 1) % g_mooncat_frame_count);
    return true;
}

static bool mooncat_tick_updates_overlay_only(void)
{
    return true;
}

static void mooncat_set_overlay_visible(bool visible)
{
    if (s_cat_image == NULL) {
        return;
    }

    if (visible) {
        lv_obj_clear_flag(s_cat_image, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_cat_image, LV_OBJ_FLAG_HIDDEN);
    }
}

static void mooncat_set_rotation(float angle_deg)
{
    s_rotation_deg = angle_deg;
    mooncat_position_overlay();
}

const clock_theme_t g_clock_theme_mooncat = {
    .name = "mooncat",
    .build = mooncat_build,
    .update = mooncat_update,
    .reset = mooncat_reset,
    .destroy_overlay = mooncat_destroy_overlay,
    .tick = mooncat_tick,
    .tick_updates_overlay_only = mooncat_tick_updates_overlay_only,
    .set_overlay_visible = mooncat_set_overlay_visible,
    .set_rotation = mooncat_set_rotation,
};
