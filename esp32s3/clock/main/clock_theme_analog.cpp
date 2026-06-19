#include "clock_theme.h"

#include "lvgl.h"

// 仿 Apple Watch「Utility」表盘：白色两级刻度、白时分针（黑描边）、橙色秒针带尾针配重、中心圆轴。
#define ANALOG_TICK_MAJOR_COLOR lv_color_hex(0xFFFFFF)
#define ANALOG_TICK_MINOR_COLOR lv_color_hex(0xB0B0B0)
#define ANALOG_HAND_COLOR lv_color_hex(0xFFFFFF)
#define ANALOG_SECOND_COLOR lv_color_hex(0xFF3B30)
#define ANALOG_OUTLINE_COLOR lv_color_hex(0x000000)
#define ANALOG_RING_COLOR lv_color_hex(0x2A2A2A)

#define ANALOG_SIZE 446
#define ANALOG_RING_SIZE 442
#define ANALOG_HOUR_LEN 120
#define ANALOG_MIN_LEN 170
#define ANALOG_SEC_LEN 182
#define ANALOG_SEC_TAIL_LEN 46

static lv_obj_t *s_scale = NULL;
static lv_obj_t *s_hour_back = NULL;
static lv_obj_t *s_hour_fg = NULL;
static lv_obj_t *s_minute_back = NULL;
static lv_obj_t *s_minute_fg = NULL;
static lv_obj_t *s_second_tail = NULL;
static lv_obj_t *s_second_back = NULL;
static lv_obj_t *s_second_fg = NULL;

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
    lv_obj_set_size(ring, ANALOG_RING_SIZE, ANALOG_RING_SIZE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, ANALOG_RING_COLOR, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_center(ring);
    return ring;
}

static lv_obj_t *analog_build(lv_obj_t *parent)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(container, ANALOG_SIZE, ANALOG_SIZE);
    lv_obj_center(container);

    make_ring(container);

    s_scale = lv_scale_create(container);
    lv_obj_set_size(s_scale, ANALOG_SIZE, ANALOG_SIZE);
    lv_obj_center(s_scale);
    lv_scale_set_mode(s_scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_label_show(s_scale, false);
    lv_scale_set_total_tick_count(s_scale, 61);
    lv_scale_set_major_tick_every(s_scale, 5);
    lv_scale_set_range(s_scale, 0, 3600);
    lv_scale_set_angle_range(s_scale, 360);
    lv_scale_set_rotation(s_scale, 270);

    // 分钟刻度：短细灰线
    lv_obj_set_style_line_color(s_scale, ANALOG_TICK_MINOR_COLOR, LV_PART_ITEMS);
    lv_obj_set_style_line_width(s_scale, 2, LV_PART_ITEMS);
    lv_obj_set_style_length(s_scale, 10, LV_PART_ITEMS);

    // 小时刻度：长粗白线
    lv_obj_set_style_line_color(s_scale, ANALOG_TICK_MAJOR_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(s_scale, 6, LV_PART_INDICATOR);
    lv_obj_set_style_length(s_scale, 34, LV_PART_INDICATOR);

    // 指针：每根「黑底描边 + 彩面」两线叠放；创建顺序决定 z 序（时针在下、分针其上、秒针最上）
    s_hour_back = make_hand(s_scale, ANALOG_OUTLINE_COLOR, 17);
    s_hour_fg = make_hand(s_scale, ANALOG_HAND_COLOR, 12);
    s_minute_back = make_hand(s_scale, ANALOG_OUTLINE_COLOR, 14);
    s_minute_fg = make_hand(s_scale, ANALOG_HAND_COLOR, 9);
    s_second_tail = make_hand(s_scale, ANALOG_SECOND_COLOR, 5);
    s_second_back = make_hand(s_scale, ANALOG_OUTLINE_COLOR, 7);
    s_second_fg = make_hand(s_scale, ANALOG_SECOND_COLOR, 4);

    // 中心圆轴：白底 + 橙心，盖在指针之上
    make_dot(s_scale, 26, ANALOG_HAND_COLOR);
    make_dot(s_scale, 12, ANALOG_SECOND_COLOR);

    return container;
}

static void analog_update(const struct tm *timeinfo)
{
    if (s_scale == NULL) {
        return;
    }

    const int32_t sec_val = timeinfo->tm_sec * 60;
    const int32_t min_val = timeinfo->tm_min * 60 + timeinfo->tm_sec;
    const int32_t hour_val = (timeinfo->tm_hour % 12) * 300 + timeinfo->tm_min * 5;
    const int32_t sec_tail_val = (sec_val + 1800) % 3600;

    lv_scale_set_line_needle_value(s_scale, s_hour_back, ANALOG_HOUR_LEN, hour_val);
    lv_scale_set_line_needle_value(s_scale, s_hour_fg, ANALOG_HOUR_LEN, hour_val);
    lv_scale_set_line_needle_value(s_scale, s_minute_back, ANALOG_MIN_LEN, min_val);
    lv_scale_set_line_needle_value(s_scale, s_minute_fg, ANALOG_MIN_LEN, min_val);
    lv_scale_set_line_needle_value(s_scale, s_second_tail, ANALOG_SEC_TAIL_LEN, sec_tail_val);
    lv_scale_set_line_needle_value(s_scale, s_second_back, ANALOG_SEC_LEN, sec_val);
    lv_scale_set_line_needle_value(s_scale, s_second_fg, ANALOG_SEC_LEN, sec_val);
}

const clock_theme_t g_clock_theme_analog = {
    .name = "analog",
    .build = analog_build,
    .update = analog_update,
};
