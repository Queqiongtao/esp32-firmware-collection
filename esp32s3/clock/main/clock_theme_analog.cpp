#include "clock_theme.h"

#include <stdio.h>

#include "lvgl.h"

// 机械计时：多层表圈、立体时标、3 点钟日期窗、6 点钟小秒盘。
#define ANALOG_BG_COLOR lv_color_hex(0x000000)
#define ANALOG_OUTER_RING_COLOR lv_color_hex(0x2A2A2A)
#define ANALOG_BEVEL_RING_COLOR lv_color_hex(0x565656)
#define ANALOG_INNER_RING_COLOR lv_color_hex(0x1F1F1F)
#define ANALOG_MINOR_TICK_COLOR lv_color_hex(0x7A7A7A)
#define ANALOG_MAJOR_TICK_COLOR lv_color_hex(0xB8B8B8)
#define ANALOG_MARKER_COLOR lv_color_hex(0xEDE9DF)
#define ANALOG_HAND_COLOR lv_color_hex(0xF3EFE5)
#define ANALOG_ACCENT_COLOR lv_color_hex(0xFF453A)
#define ANALOG_OUTLINE_COLOR lv_color_hex(0x000000)
#define ANALOG_DATE_FRAME_COLOR lv_color_hex(0x242424)
#define ANALOG_DATE_BORDER_COLOR lv_color_hex(0x4A4A4A)
#define ANALOG_DATE_COLOR lv_color_hex(0xF0E6D2)
#define ANALOG_WEEKDAY_COLOR lv_color_hex(0x9A9A9A)
#define ANALOG_LABEL_COLOR lv_color_hex(0x5F5F5F)

#define ANALOG_SIZE 466
#define ANALOG_HOUR_LEN 112
#define ANALOG_MIN_LEN 150
#define ANALOG_SUB_SEC_LEN 36
#define ANALOG_SUB_SEC_TAIL_LEN 10
#define ANALOG_SUB_DIAL_OFFSET_Y 115
#define ANALOG_DATE_OFFSET_X 126

static lv_obj_t *s_scale = NULL;
static lv_obj_t *s_sub_scale = NULL;
static lv_obj_t *s_date_day = NULL;
static lv_obj_t *s_date_weekday = NULL;
static lv_obj_t *s_hour_back = NULL;
static lv_obj_t *s_hour_fg = NULL;
static lv_obj_t *s_minute_back = NULL;
static lv_obj_t *s_minute_fg = NULL;
static lv_obj_t *s_sub_second_tail = NULL;
static lv_obj_t *s_sub_second_back = NULL;
static lv_obj_t *s_sub_second_fg = NULL;

namespace {

static const char *const kWeekdayAbbr[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT",
};

static lv_obj_t *make_box(lv_obj_t *parent,
                          int32_t width,
                          int32_t height,
                          lv_color_t color,
                          lv_opa_t opa,
                          int32_t radius)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, width, height);
    lv_obj_set_style_radius(box, radius, 0);
    lv_obj_set_style_bg_color(box, color, 0);
    lv_obj_set_style_bg_opa(box, opa, 0);
    return box;
}

static lv_obj_t *make_ring(lv_obj_t *parent,
                           int32_t size,
                           int32_t width,
                           lv_color_t color)
{
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ring, size, size);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, color, 0);
    lv_obj_set_style_border_width(ring, width, 0);
    lv_obj_center(ring);
    return ring;
}

static lv_obj_t *make_dot(lv_obj_t *parent, int32_t size, lv_color_t color)
{
    lv_obj_t *dot = make_box(parent, size, size, color, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_center(dot);
    return dot;
}

static lv_obj_t *make_hand(lv_obj_t *scale, lv_color_t color, int32_t width)
{
    lv_obj_t *hand = lv_line_create(scale);
    lv_obj_set_style_line_color(hand, color, 0);
    lv_obj_set_style_line_width(hand, width, 0);
    lv_obj_set_style_line_rounded(hand, true, 0);
    return hand;
}

static lv_obj_t *make_minute_scale(lv_obj_t *parent)
{
    lv_obj_t *scale = lv_scale_create(parent);
    lv_obj_set_size(scale, 426, 426);
    lv_obj_center(scale);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_label_show(scale, false);
    lv_scale_set_total_tick_count(scale, 61);
    lv_scale_set_major_tick_every(scale, 5);
    lv_scale_set_range(scale, 0, 3600);
    lv_scale_set_angle_range(scale, 360);
    lv_scale_set_rotation(scale, 270);

    lv_obj_set_style_line_color(scale, ANALOG_MINOR_TICK_COLOR, LV_PART_ITEMS);
    lv_obj_set_style_line_width(scale, 1, LV_PART_ITEMS);
    lv_obj_set_style_length(scale, 7, LV_PART_ITEMS);

    lv_obj_set_style_line_color(scale, ANALOG_MAJOR_TICK_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(scale, 2, LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, 11, LV_PART_INDICATOR);
    return scale;
}

static lv_obj_t *make_hour_marker_scale(lv_obj_t *parent)
{
    lv_obj_t *scale = lv_scale_create(parent);
    lv_obj_set_size(scale, 396, 396);
    lv_obj_center(scale);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_label_show(scale, false);
    lv_scale_set_total_tick_count(scale, 13);
    lv_scale_set_major_tick_every(scale, 1);
    lv_scale_set_range(scale, 0, 12);
    lv_scale_set_angle_range(scale, 360);
    lv_scale_set_rotation(scale, 270);

    lv_obj_set_style_line_color(scale, ANALOG_MARKER_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(scale, 7, LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, 15, LV_PART_INDICATOR);
    lv_obj_set_style_line_rounded(scale, true, LV_PART_INDICATOR);
    return scale;
}

static lv_obj_t *make_date_window(lv_obj_t *parent)
{
    lv_obj_t *frame = make_box(parent, 82, 62, ANALOG_DATE_FRAME_COLOR, LV_OPA_COVER, 7);
    lv_obj_align(frame, LV_ALIGN_CENTER, ANALOG_DATE_OFFSET_X, 0);

    lv_obj_t *window = make_box(frame, 74, 54, ANALOG_BG_COLOR, LV_OPA_COVER, 4);
    lv_obj_set_style_border_width(window, 1, 0);
    lv_obj_set_style_border_color(window, ANALOG_DATE_BORDER_COLOR, 0);
    lv_obj_set_style_border_opa(window, LV_OPA_COVER, 0);
    lv_obj_center(window);

    s_date_weekday = lv_label_create(window);
    lv_obj_set_style_text_font(s_date_weekday, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_date_weekday, ANALOG_WEEKDAY_COLOR, 0);
    lv_obj_set_style_text_letter_space(s_date_weekday, 1, 0);
    lv_label_set_text(s_date_weekday, "---");
    lv_obj_align(s_date_weekday, LV_ALIGN_TOP_MID, 0, 6);

    s_date_day = lv_label_create(window);
    lv_obj_set_style_text_font(s_date_day, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_date_day, ANALOG_DATE_COLOR, 0);
    lv_label_set_text(s_date_day, "--");
    lv_obj_align(s_date_day, LV_ALIGN_TOP_MID, 0, 19);
    return window;
}

static lv_obj_t *make_sub_dial(lv_obj_t *parent)
{
    lv_obj_t *frame = make_box(parent, 112, 112, ANALOG_DATE_FRAME_COLOR, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_align(frame, LV_ALIGN_CENTER, 0, ANALOG_SUB_DIAL_OFFSET_Y);

    lv_obj_t *window = make_box(frame, 104, 104, ANALOG_BG_COLOR, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(window, 1, 0);
    lv_obj_set_style_border_color(window, ANALOG_DATE_BORDER_COLOR, 0);
    lv_obj_set_style_border_opa(window, LV_OPA_COVER, 0);
    lv_obj_center(window);

    s_sub_scale = lv_scale_create(window);
    lv_obj_set_size(s_sub_scale, 100, 100);
    lv_obj_center(s_sub_scale);
    lv_scale_set_mode(s_sub_scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_label_show(s_sub_scale, false);
    lv_scale_set_total_tick_count(s_sub_scale, 61);
    lv_scale_set_major_tick_every(s_sub_scale, 5);
    lv_scale_set_range(s_sub_scale, 0, 3600);
    lv_scale_set_angle_range(s_sub_scale, 360);
    lv_scale_set_rotation(s_sub_scale, 270);

    lv_obj_set_style_line_color(s_sub_scale, ANALOG_MINOR_TICK_COLOR, LV_PART_ITEMS);
    lv_obj_set_style_line_width(s_sub_scale, 1, LV_PART_ITEMS);
    lv_obj_set_style_length(s_sub_scale, 4, LV_PART_ITEMS);

    lv_obj_set_style_line_color(s_sub_scale, ANALOG_MAJOR_TICK_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(s_sub_scale, 2, LV_PART_INDICATOR);
    lv_obj_set_style_length(s_sub_scale, 7, LV_PART_INDICATOR);

    s_sub_second_tail = make_hand(s_sub_scale, ANALOG_ACCENT_COLOR, 2);
    s_sub_second_back = make_hand(s_sub_scale, ANALOG_OUTLINE_COLOR, 4);
    s_sub_second_fg = make_hand(s_sub_scale, ANALOG_ACCENT_COLOR, 2);

    make_dot(s_sub_scale, 10, ANALOG_HAND_COLOR);
    make_dot(s_sub_scale, 4, ANALOG_ACCENT_COLOR);
    return window;
}

} // namespace

static lv_obj_t *analog_build(lv_obj_t *parent)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(container, ANALOG_SIZE, ANALOG_SIZE);
    lv_obj_set_style_bg_color(container, ANALOG_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_center(container);

    // 多层表圈：外缘、亮色倒角、内圈，形成深浅过渡。
    make_ring(container, 458, 1, ANALOG_OUTER_RING_COLOR);
    make_ring(container, 446, 2, ANALOG_BEVEL_RING_COLOR);
    make_ring(container, 432, 1, ANALOG_INNER_RING_COLOR);

    s_scale = make_minute_scale(container);
    make_hour_marker_scale(container);

    lv_obj_t *brand = lv_label_create(container);
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(brand, ANALOG_LABEL_COLOR, 0);
    lv_obj_set_style_text_letter_space(brand, 3, 0);
    lv_label_set_text(brand, "MECHANICAL");
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 154);

    make_date_window(container);
    make_sub_dial(container);

    // 指针：黑底描边 + 米白针面；日期窗和小秒盘在指针下方。
    s_hour_back = make_hand(s_scale, ANALOG_OUTLINE_COLOR, 17);
    s_hour_fg = make_hand(s_scale, ANALOG_HAND_COLOR, 11);
    s_minute_back = make_hand(s_scale, ANALOG_OUTLINE_COLOR, 14);
    s_minute_fg = make_hand(s_scale, ANALOG_HAND_COLOR, 8);

    // 三层中心轴：深色基座、米白轴、红色轴心。
    make_dot(s_scale, 30, ANALOG_DATE_FRAME_COLOR);
    make_dot(s_scale, 20, ANALOG_HAND_COLOR);
    make_dot(s_scale, 8, ANALOG_ACCENT_COLOR);

    return container;
}

static bool analog_update(const struct tm *timeinfo)
{
    if (s_scale == NULL || s_sub_scale == NULL) {
        return false;
    }

    const int32_t min_val = timeinfo->tm_min * 60 + timeinfo->tm_sec;
    const int32_t hour_val = (timeinfo->tm_hour % 12) * 300 +
                             timeinfo->tm_min * 5 +
                             timeinfo->tm_sec / 12;
    const int32_t sec_val = timeinfo->tm_sec * 60;
    const int32_t sec_tail_val = (sec_val + 1800) % 3600;

    lv_scale_set_line_needle_value(s_scale, s_hour_back, ANALOG_HOUR_LEN, hour_val);
    lv_scale_set_line_needle_value(s_scale, s_hour_fg, ANALOG_HOUR_LEN, hour_val);
    lv_scale_set_line_needle_value(s_scale, s_minute_back, ANALOG_MIN_LEN, min_val);
    lv_scale_set_line_needle_value(s_scale, s_minute_fg, ANALOG_MIN_LEN, min_val);
    lv_scale_set_line_needle_value(s_sub_scale, s_sub_second_tail, ANALOG_SUB_SEC_TAIL_LEN, sec_tail_val);
    lv_scale_set_line_needle_value(s_sub_scale, s_sub_second_back, ANALOG_SUB_SEC_LEN, sec_val);
    lv_scale_set_line_needle_value(s_sub_scale, s_sub_second_fg, ANALOG_SUB_SEC_LEN, sec_val);

    char weekday_text[8] = {};
    char day_text[4] = {};
    snprintf(weekday_text, sizeof(weekday_text), "%s", kWeekdayAbbr[timeinfo->tm_wday % 7]);
    snprintf(day_text, sizeof(day_text), "%02d", timeinfo->tm_mday);
    lv_label_set_text(s_date_weekday, weekday_text);
    lv_label_set_text(s_date_day, day_text);
    return true;
}

const clock_theme_t g_clock_theme_analog = {
    .name = "analog",
    .build = analog_build,
    .update = analog_update,
};
