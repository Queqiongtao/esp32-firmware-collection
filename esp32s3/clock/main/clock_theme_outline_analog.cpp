#include "clock_theme.h"

#include "lvgl.h"

// 黑底白线极简模拟盘：弱化刻度，保留边框、细针和红色秒针。
#define OUTLINE_BG_COLOR lv_color_hex(0x000000)
#define OUTLINE_RING_COLOR lv_color_hex(0x404040)
#define OUTLINE_MAJOR_COLOR lv_color_hex(0xFFFFFF)
#define OUTLINE_MINOR_COLOR lv_color_hex(0x6E6E6E)
#define OUTLINE_HAND_COLOR lv_color_hex(0xFFFFFF)
#define OUTLINE_SECOND_COLOR lv_color_hex(0xFF453A)
#define OUTLINE_OUTLINE_COLOR lv_color_hex(0x000000)

#define OUTLINE_SIZE 446
#define OUTLINE_RING_SIZE 424
#define OUTLINE_HOUR_LEN 116
#define OUTLINE_MIN_LEN 164
#define OUTLINE_SEC_LEN 180
#define OUTLINE_SEC_TAIL_LEN 42

static lv_obj_t *s_scale = NULL;
static lv_obj_t *s_ring = NULL;
static lv_obj_t *s_hour_back = NULL;
static lv_obj_t *s_hour_fg = NULL;
static lv_obj_t *s_minute_back = NULL;
static lv_obj_t *s_minute_fg = NULL;
static lv_obj_t *s_second_tail = NULL;
static lv_obj_t *s_second_back = NULL;
static lv_obj_t *s_second_fg = NULL;
static lv_obj_t *s_center_outer = NULL;
static lv_obj_t *s_center_inner = NULL;

static lv_obj_t *make_hand(lv_obj_t *scale, lv_color_t color, int32_t width)
{
    lv_obj_t *hand = lv_line_create(scale);
    lv_obj_set_style_line_color(hand, color, 0);
    lv_obj_set_style_line_width(hand, width, 0);
    lv_obj_set_style_line_rounded(hand, true, 0);
    return hand;
}

static lv_obj_t *make_dot(lv_obj_t *parent, int32_t size, lv_color_t color)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, color, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_center(dot);
    return dot;
}

static lv_obj_t *make_ring(lv_obj_t *parent)
{
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, OUTLINE_RING_SIZE, OUTLINE_RING_SIZE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, OUTLINE_RING_COLOR, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_center(ring);
    return ring;
}

static lv_obj_t *outline_analog_build(lv_obj_t *parent)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(container, OUTLINE_SIZE, OUTLINE_SIZE);
    lv_obj_set_style_bg_color(container, OUTLINE_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_center(container);

    s_ring = make_ring(container);

    s_scale = lv_scale_create(container);
    lv_obj_set_size(s_scale, OUTLINE_SIZE, OUTLINE_SIZE);
    lv_obj_center(s_scale);
    lv_scale_set_mode(s_scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_label_show(s_scale, false);
    lv_scale_set_total_tick_count(s_scale, 61);
    lv_scale_set_major_tick_every(s_scale, 5);
    lv_scale_set_range(s_scale, 0, 3600);
    lv_scale_set_angle_range(s_scale, 360);
    lv_scale_set_rotation(s_scale, 270);

    lv_obj_set_style_line_color(s_scale, OUTLINE_MINOR_COLOR, LV_PART_ITEMS);
    lv_obj_set_style_line_width(s_scale, 1, LV_PART_ITEMS);
    lv_obj_set_style_length(s_scale, 8, LV_PART_ITEMS);

    lv_obj_set_style_line_color(s_scale, OUTLINE_MAJOR_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(s_scale, 4, LV_PART_INDICATOR);
    lv_obj_set_style_length(s_scale, 26, LV_PART_INDICATOR);

    s_hour_back = make_hand(s_scale, OUTLINE_OUTLINE_COLOR, 15);
    s_hour_fg = make_hand(s_scale, OUTLINE_HAND_COLOR, 9);
    s_minute_back = make_hand(s_scale, OUTLINE_OUTLINE_COLOR, 12);
    s_minute_fg = make_hand(s_scale, OUTLINE_HAND_COLOR, 7);
    s_second_tail = make_hand(s_scale, OUTLINE_SECOND_COLOR, 4);
    s_second_back = make_hand(s_scale, OUTLINE_OUTLINE_COLOR, 6);
    s_second_fg = make_hand(s_scale, OUTLINE_SECOND_COLOR, 3);

    s_center_outer = make_dot(s_scale, 24, OUTLINE_HAND_COLOR);
    s_center_inner = make_dot(s_scale, 10, OUTLINE_SECOND_COLOR);

    return container;
}

static bool outline_analog_update(const struct tm *timeinfo)
{
    if (s_scale == NULL) {
        return false;
    }

    const int32_t sec_val = timeinfo->tm_sec * 60;
    const int32_t min_val = timeinfo->tm_min * 60 + timeinfo->tm_sec;
    const int32_t hour_val = (timeinfo->tm_hour % 12) * 300 + timeinfo->tm_min * 5;
    const int32_t sec_tail_val = (sec_val + 1800) % 3600;

    lv_scale_set_line_needle_value(s_scale, s_hour_back, OUTLINE_HOUR_LEN, hour_val);
    lv_scale_set_line_needle_value(s_scale, s_hour_fg, OUTLINE_HOUR_LEN, hour_val);
    lv_scale_set_line_needle_value(s_scale, s_minute_back, OUTLINE_MIN_LEN, min_val);
    lv_scale_set_line_needle_value(s_scale, s_minute_fg, OUTLINE_MIN_LEN, min_val);
    lv_scale_set_line_needle_value(s_scale, s_second_tail, OUTLINE_SEC_TAIL_LEN, sec_tail_val);
    lv_scale_set_line_needle_value(s_scale, s_second_back, OUTLINE_SEC_LEN, sec_val);
    lv_scale_set_line_needle_value(s_scale, s_second_fg, OUTLINE_SEC_LEN, sec_val);

    (void)s_ring;
    (void)s_center_outer;
    (void)s_center_inner;
    return true;
}

const clock_theme_t g_clock_theme_outline_analog = {
    .name = "outline",
    .build = outline_analog_build,
    .update = outline_analog_update,
};
