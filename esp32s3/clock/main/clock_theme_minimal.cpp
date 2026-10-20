#include "clock_theme.h"

#include <cmath>
#include <cstddef>
#include <stdio.h>

#include "lvgl.h"

extern "C" const lv_font_t lv_font_clock_144;

namespace {

constexpr int32_t THEME_SIZE = 466;
constexpr int32_t SECONDS_ARC_SIZE = 436;
constexpr int32_t INNER_RING_SIZE = 412;
constexpr int32_t BEAD_MASK_SIZE = 14;
constexpr int32_t BEAD_DOT_SIZE = 10;
constexpr float BEAD_RADIUS = 216.5f;
constexpr int32_t TIME_OFFSET_Y = -33;
constexpr int32_t INFO_OFFSET_Y = 72;
constexpr int32_t INFO_GAP = 12;
constexpr int32_t INFO_DOT_SIZE = 3;

#define MINIMAL_BG_COLOR lv_color_hex(0x000000)
#define MINIMAL_TIME_COLOR lv_color_hex(0xFFFFFF)
#define MINIMAL_ACCENT_COLOR lv_color_hex(0xFF9F0A)
#define MINIMAL_DATE_COLOR lv_color_hex(0x9AA0A6)
#define MINIMAL_ARC_TRACK_COLOR lv_color_hex(0x232323)
#define MINIMAL_INNER_RING_COLOR lv_color_hex(0x2A2A2A)

static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_seconds_label = NULL;
static lv_obj_t *s_date_label = NULL;
static lv_obj_t *s_seconds_arc = NULL;
static lv_obj_t *s_bead = NULL;
static lv_obj_t *s_bead_dot = NULL;
static lv_obj_t *s_info_dot = NULL;
static uint32_t s_accent_hex = 0;

static const uint32_t kAccentPalette[] = {
    0xFF9F0A, // 橙
    0xFF6B4A, // 珊瑚
    0xFF4D8D, // 玫红
    0xD96BFF, // 兰紫
    0x8B7BFF, // 紫蓝
    0x4D9DFF, // 蓝
    0x2FD6D0, // 青
    0x6BE3A8, // 薄荷
};

constexpr size_t kAccentCount = sizeof(kAccentPalette) / sizeof(kAccentPalette[0]);

static const char *const kWeekdayAbbr[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

static const char *const kMonthAbbr[12] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

static lv_obj_t *make_seconds_arc(lv_obj_t *parent)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(arc, SECONDS_ARC_SIZE, SECONDS_ARC_SIZE);
    lv_obj_center(arc);

    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_range(arc, 0, 60);
    lv_arc_set_value(arc, 0);

    lv_obj_set_style_arc_width(arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, MINIMAL_ARC_TRACK_COLOR, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);

    lv_obj_set_style_arc_width(arc, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, MINIMAL_ACCENT_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);

    return arc;
}

static void make_inner_ring(lv_obj_t *parent)
{
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ring, INNER_RING_SIZE, INNER_RING_SIZE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, MINIMAL_INNER_RING_COLOR, 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
    lv_obj_center(ring);
}

static lv_obj_t *make_bead(lv_obj_t *parent)
{
    lv_obj_t *bead = lv_obj_create(parent);
    lv_obj_remove_style_all(bead);
    lv_obj_clear_flag(bead, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bead, BEAD_MASK_SIZE, BEAD_MASK_SIZE);
    lv_obj_set_style_radius(bead, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bead, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bead, LV_OPA_COVER, 0);

    lv_obj_t *dot = lv_obj_create(bead);
    lv_obj_remove_style_all(dot);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dot, BEAD_DOT_SIZE, BEAD_DOT_SIZE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, MINIMAL_ACCENT_COLOR, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_center(dot);
    s_bead_dot = dot;

    lv_obj_align(bead, LV_ALIGN_CENTER, 0, -(int32_t)BEAD_RADIUS);
    return bead;
}

static lv_obj_t *make_info_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, INFO_GAP, 0);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, INFO_OFFSET_Y);
    return row;
}

} // namespace

static lv_obj_t *minimal_build(lv_obj_t *parent)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(container, THEME_SIZE, THEME_SIZE);
    lv_obj_set_style_bg_color(container, MINIMAL_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_center(container);

    s_seconds_arc = make_seconds_arc(container);
    make_inner_ring(container);
    s_bead = make_bead(container);

    s_time_label = lv_label_create(container);
    lv_obj_set_style_text_color(s_time_label, MINIMAL_TIME_COLOR, 0);
    lv_obj_set_style_text_font(s_time_label, &lv_font_clock_144, 0);
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, TIME_OFFSET_Y);
    lv_label_set_text(s_time_label, "--:--");

    lv_obj_t *info_row = make_info_row(container);

    s_date_label = lv_label_create(info_row);
    lv_obj_set_style_text_color(s_date_label, MINIMAL_DATE_COLOR, 0);
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_letter_space(s_date_label, 2, 0);
    lv_label_set_text(s_date_label, "--- -- ---");

    lv_obj_t *dot = lv_obj_create(info_row);
    lv_obj_remove_style_all(dot);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dot, INFO_DOT_SIZE, INFO_DOT_SIZE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, MINIMAL_ACCENT_COLOR, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_70, 0);
    s_info_dot = dot;

    s_seconds_label = lv_label_create(info_row);
    lv_obj_set_style_text_color(s_seconds_label, MINIMAL_ACCENT_COLOR, 0);
    lv_obj_set_style_text_font(s_seconds_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_letter_space(s_seconds_label, 1, 0);
    lv_label_set_text(s_seconds_label, "--");

    s_accent_hex = 0;
    return container;
}

static bool minimal_update(const struct tm *timeinfo)
{
    char time_text[8] = {};
    char sec_text[4] = {};
    char date_text[16] = {};

    snprintf(time_text, sizeof(time_text), "%02d:%02d",
             timeinfo->tm_hour, timeinfo->tm_min);
    snprintf(sec_text, sizeof(sec_text), "%02d", timeinfo->tm_sec);
    snprintf(date_text, sizeof(date_text), "%s %02d %s",
             kWeekdayAbbr[timeinfo->tm_wday % 7],
             timeinfo->tm_mday,
             kMonthAbbr[timeinfo->tm_mon % 12]);

    lv_label_set_text(s_time_label, time_text);
    lv_label_set_text(s_seconds_label, sec_text);
    lv_label_set_text(s_date_label, date_text);
    lv_arc_set_value(s_seconds_arc, timeinfo->tm_sec);

    const uint32_t accent_hex = kAccentPalette[timeinfo->tm_min % kAccentCount];
    if (accent_hex != s_accent_hex) {
        s_accent_hex = accent_hex;
        const lv_color_t accent = lv_color_hex(accent_hex);
        lv_obj_set_style_arc_color(s_seconds_arc, accent, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s_seconds_label, accent, 0);
        lv_obj_set_style_bg_color(s_info_dot, accent, 0);
        lv_obj_set_style_bg_color(s_bead_dot, accent, 0);
    }

    const float rad = (-90.0f + timeinfo->tm_sec * 6.0f) * 3.14159265f / 180.0f;
    const int32_t bead_dx = static_cast<int32_t>(std::lround(std::cos(rad) * BEAD_RADIUS));
    const int32_t bead_dy = static_cast<int32_t>(std::lround(std::sin(rad) * BEAD_RADIUS));
    lv_obj_align(s_bead, LV_ALIGN_CENTER, bead_dx, bead_dy);
    return true;
}

const clock_theme_t g_clock_theme_minimal = {
    .name = "minimal",
    .build = minimal_build,
    .update = minimal_update,
};
