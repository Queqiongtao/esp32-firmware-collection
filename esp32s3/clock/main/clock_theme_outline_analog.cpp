#include "clock_theme.h"

#include <cmath>
#include <stdio.h>

#include "lvgl.h"

// 线框轨道：多同心细环、低透明放射刻度、沿表圈运行的红色秒点、空心中心。
#define OUTLINE_BG_COLOR lv_color_hex(0x000000)
#define OUTLINE_OUTER_RING_COLOR lv_color_hex(0x2E3A40)
#define OUTLINE_MAIN_RING_COLOR lv_color_hex(0x52666F)
#define OUTLINE_MID_RING_COLOR lv_color_hex(0x263238)
#define OUTLINE_INNER_RING_COLOR lv_color_hex(0x1B2428)
#define OUTLINE_SUNBURST_COLOR lv_color_hex(0x2B373C)
#define OUTLINE_MINOR_TICK_COLOR lv_color_hex(0x4A5D66)
#define OUTLINE_MAJOR_TICK_COLOR lv_color_hex(0x7C929C)
#define OUTLINE_HOUR_MARKER_COLOR lv_color_hex(0x8FA6B0)
#define OUTLINE_HAND_COLOR lv_color_hex(0xE6F1F5)
#define OUTLINE_HUB_COLOR lv_color_hex(0x9FB4BE)
#define OUTLINE_HUB_INNER_COLOR lv_color_hex(0x4A5E67)
#define OUTLINE_ACCENT_COLOR lv_color_hex(0xFF3B30)
#define OUTLINE_DATE_RING_COLOR lv_color_hex(0x38474E)
#define OUTLINE_DATE_COLOR lv_color_hex(0xDCE8EC)
#define OUTLINE_WEEKDAY_COLOR lv_color_hex(0x72858E)

#define OUTLINE_SIZE 466
#define OUTLINE_HOUR_LEN 110
#define OUTLINE_MIN_LEN 152
#define OUTLINE_SECOND_BEAD_RADIUS 206.0f
#define OUTLINE_SECOND_BEAD_SIZE 12
#define OUTLINE_SECOND_DOT_SIZE 8
#define OUTLINE_DATE_OFFSET_Y 112

static lv_obj_t *s_scale = NULL;
static lv_obj_t *s_date_day = NULL;
static lv_obj_t *s_date_weekday = NULL;
static lv_obj_t *s_hour_fg = NULL;
static lv_obj_t *s_minute_fg = NULL;
static lv_obj_t *s_second_bead = NULL;

namespace {

static const char *const kWeekdayAbbr[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT",
};

static const char *const kMonthAbbr[12] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
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

static lv_obj_t *make_hand(lv_obj_t *scale, int32_t width)
{
    lv_obj_t *hand = lv_line_create(scale);
    lv_obj_set_style_line_color(hand, OUTLINE_HAND_COLOR, 0);
    lv_obj_set_style_line_width(hand, width, 0);
    lv_obj_set_style_line_rounded(hand, true, 0);
    return hand;
}

static lv_obj_t *make_sunburst_scale(lv_obj_t *parent)
{
    lv_obj_t *scale = lv_scale_create(parent);
    lv_obj_set_size(scale, 430, 430);
    lv_obj_center(scale);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_label_show(scale, false);
    lv_scale_set_total_tick_count(scale, 121);
    lv_scale_set_major_tick_every(scale, 6);
    lv_scale_set_range(scale, 0, 3600);
    lv_scale_set_angle_range(scale, 360);
    lv_scale_set_rotation(scale, 270);

    lv_obj_set_style_line_color(scale, OUTLINE_SUNBURST_COLOR, LV_PART_ITEMS);
    lv_obj_set_style_line_width(scale, 1, LV_PART_ITEMS);
    lv_obj_set_style_length(scale, 18, LV_PART_ITEMS);

    lv_obj_set_style_line_color(scale, OUTLINE_SUNBURST_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(scale, 1, LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, 18, LV_PART_INDICATOR);
    return scale;
}

static lv_obj_t *make_minute_scale(lv_obj_t *parent)
{
    lv_obj_t *scale = lv_scale_create(parent);
    lv_obj_set_size(scale, 430, 430);
    lv_obj_center(scale);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_label_show(scale, false);
    lv_scale_set_total_tick_count(scale, 61);
    lv_scale_set_major_tick_every(scale, 5);
    lv_scale_set_range(scale, 0, 3600);
    lv_scale_set_angle_range(scale, 360);
    lv_scale_set_rotation(scale, 270);

    lv_obj_set_style_line_color(scale, OUTLINE_MINOR_TICK_COLOR, LV_PART_ITEMS);
    lv_obj_set_style_line_width(scale, 1, LV_PART_ITEMS);
    lv_obj_set_style_length(scale, 5, LV_PART_ITEMS);

    lv_obj_set_style_line_color(scale, OUTLINE_MAJOR_TICK_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(scale, 1, LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, 8, LV_PART_INDICATOR);
    return scale;
}

static lv_obj_t *make_hour_marker_scale(lv_obj_t *parent)
{
    lv_obj_t *scale = lv_scale_create(parent);
    lv_obj_set_size(scale, 380, 380);
    lv_obj_center(scale);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_label_show(scale, false);
    lv_scale_set_total_tick_count(scale, 13);
    lv_scale_set_major_tick_every(scale, 1);
    lv_scale_set_range(scale, 0, 12);
    lv_scale_set_angle_range(scale, 360);
    lv_scale_set_rotation(scale, 270);

    lv_obj_set_style_line_color(scale, OUTLINE_HOUR_MARKER_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(scale, 2, LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, 10, LV_PART_INDICATOR);
    lv_obj_set_style_line_rounded(scale, true, LV_PART_INDICATOR);
    return scale;
}

static lv_obj_t *make_date_ring(lv_obj_t *parent)
{
    lv_obj_t *ring = make_ring(parent, 104, 1, OUTLINE_DATE_RING_COLOR);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, OUTLINE_DATE_OFFSET_Y);

    s_date_weekday = lv_label_create(ring);
    lv_obj_set_style_text_font(s_date_weekday, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_date_weekday, OUTLINE_WEEKDAY_COLOR, 0);
    lv_obj_set_style_text_letter_space(s_date_weekday, 2, 0);
    lv_label_set_text(s_date_weekday, "---");
    lv_obj_align(s_date_weekday, LV_ALIGN_CENTER, 0, -15);

    s_date_day = lv_label_create(ring);
    lv_obj_set_style_text_font(s_date_day, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_date_day, OUTLINE_DATE_COLOR, 0);
    lv_label_set_text(s_date_day, "-- ---");
    lv_obj_align(s_date_day, LV_ALIGN_CENTER, 0, 8);
    return ring;
}

static lv_obj_t *make_second_bead(lv_obj_t *parent)
{
    lv_obj_t *bead = make_box(parent,
                              OUTLINE_SECOND_BEAD_SIZE,
                              OUTLINE_SECOND_BEAD_SIZE,
                              OUTLINE_BG_COLOR,
                              LV_OPA_COVER,
                              LV_RADIUS_CIRCLE);
    lv_obj_t *dot = make_box(bead,
                             OUTLINE_SECOND_DOT_SIZE,
                             OUTLINE_SECOND_DOT_SIZE,
                             OUTLINE_ACCENT_COLOR,
                             LV_OPA_COVER,
                             LV_RADIUS_CIRCLE);
    lv_obj_center(dot);
    lv_obj_align(bead, LV_ALIGN_CENTER, 0, -(int32_t)OUTLINE_SECOND_BEAD_RADIUS);
    return bead;
}

} // namespace

static lv_obj_t *outline_analog_build(lv_obj_t *parent)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(container, OUTLINE_SIZE, OUTLINE_SIZE);
    lv_obj_set_style_bg_color(container, OUTLINE_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_center(container);

    make_ring(container, 458, 1, OUTLINE_OUTER_RING_COLOR);
    make_ring(container, 430, 1, OUTLINE_MAIN_RING_COLOR);
    make_ring(container, 398, 1, OUTLINE_MID_RING_COLOR);
    make_ring(container, 356, 1, OUTLINE_INNER_RING_COLOR);

    make_sunburst_scale(container);
    s_scale = make_minute_scale(container);
    make_hour_marker_scale(container);
    make_date_ring(container);

    s_hour_fg = make_hand(s_scale, 5);
    s_minute_fg = make_hand(s_scale, 3);
    s_second_bead = make_second_bead(container);

    // 空心中心：双层细环，指针在环内交汇。
    make_ring(s_scale, 18, 1, OUTLINE_HUB_COLOR);
    make_ring(s_scale, 8, 1, OUTLINE_HUB_INNER_COLOR);

    return container;
}

static bool outline_analog_update(const struct tm *timeinfo)
{
    if (s_scale == NULL) {
        return false;
    }

    const int32_t min_val = timeinfo->tm_min * 60 + timeinfo->tm_sec;
    const int32_t hour_val = (timeinfo->tm_hour % 12) * 300 +
                             timeinfo->tm_min * 5 +
                             timeinfo->tm_sec / 12;

    lv_scale_set_line_needle_value(s_scale, s_hour_fg, OUTLINE_HOUR_LEN, hour_val);
    lv_scale_set_line_needle_value(s_scale, s_minute_fg, OUTLINE_MIN_LEN, min_val);

    const float rad = (-90.0f + timeinfo->tm_sec * 6.0f) * 3.14159265f / 180.0f;
    const int32_t bead_dx = static_cast<int32_t>(std::lround(std::cos(rad) * OUTLINE_SECOND_BEAD_RADIUS));
    const int32_t bead_dy = static_cast<int32_t>(std::lround(std::sin(rad) * OUTLINE_SECOND_BEAD_RADIUS));
    lv_obj_align(s_second_bead, LV_ALIGN_CENTER, bead_dx, bead_dy);

    char weekday_text[8] = {};
    char date_text[16] = {};
    snprintf(weekday_text, sizeof(weekday_text), "%s", kWeekdayAbbr[timeinfo->tm_wday % 7]);
    snprintf(date_text, sizeof(date_text), "%02d %s",
             timeinfo->tm_mday,
             kMonthAbbr[timeinfo->tm_mon % 12]);
    lv_label_set_text(s_date_weekday, weekday_text);
    lv_label_set_text(s_date_day, date_text);

    return true;
}

const clock_theme_t g_clock_theme_outline_analog = {
    .name = "outline",
    .build = outline_analog_build,
    .update = outline_analog_update,
};
