#include "clock_theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "m5_watch_assets.h"

namespace {

constexpr int32_t THEME_SIZE = 466;
constexpr int32_t CENTER = 233;
constexpr float PI = 3.14159265358979323846f;

static const char *const kWeekdayAbbr[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

struct M5Theme {
    uint32_t background_color;
    uint32_t accent_color;
    uint32_t time_color;
    uint32_t date_color;
};

static const M5Theme kSimpleThemes[10] = {
    {0x000000, 0xA0A0A0, 0xFFFFFF, 0xA0A0A0},
    {0xEEEEEE, 0x525252, 0x484848, 0x525252},
    {0xC2EFEB, 0x6A938F, 0x566E6C, 0x6A938F},
    {0xEEE0CB, 0x948063, 0x6A5E4D, 0x948063},
    {0xB1CC74, 0x668323, 0x4B5E20, 0x668323},
    {0x8A89C0, 0xCBCBFF, 0xDFDEFF, 0xCBCBFF},
    {0xF26419, 0xFFD3BC, 0xFFEFE6, 0xFFD3BC},
    {0xF2D7EE, 0xA4789D, 0x83627E, 0xA4789D},
    {0x048A81, 0x70CFC9, 0xB1FFFA, 0x70CFC9},
    {0x9DB4C0, 0xDCF3FF, 0xECF8FF, 0xDCF3FF},
};

struct M5FlowTheme {
    uint32_t background_color;
    uint32_t panel_color;
    uint32_t time_color;
    uint32_t date_color;
};

static const M5FlowTheme kNumberFlowThemes[10] = {
    {0x000000, 0x1A1919, 0xFFFFFF, 0xA0A0A0},
    {0xEEEEEE, 0xE4E3E3, 0x484848, 0x525252},
    {0xC2EFEB, 0xB1E1DD, 0x566E6C, 0x6A938F},
    {0xEEE0CB, 0xD9CAB2, 0x6A5E4D, 0x948063},
    {0xB1CC74, 0xA4BD6B, 0x4B5E20, 0x668323},
    {0x8A89C0, 0x8180B4, 0xDFDEFF, 0xCBCBFF},
    {0xF26419, 0xF6722C, 0xFFEFE6, 0xFFD3BC},
    {0xF2D7EE, 0xE9C9E5, 0x83627E, 0xA4789D},
    {0x048A81, 0x05968C, 0xB1FFFA, 0x70CFC9},
    {0x9DB4C0, 0x92A8B3, 0xECF8FF, 0xDCF3FF},
};

static const uint32_t kBigNumberPalettes[4][4] = {
    {0xCBDB8C, 0xD3EA71, 0xE9F5B8, 0x94A350},
    {0xD1F3BF, 0xCDFCB9, 0xF8FEE9, 0x84D86D},
    {0xDB9D7D, 0xE8D780, 0xF4EBC0, 0xF2B050},
    {0xB49EDB, 0x9BC1FF, 0xC4C9FA, 0xAF94E7},
};

static lv_obj_t *make_root(lv_obj_t *parent, lv_color_t bg)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(container, THEME_SIZE, THEME_SIZE);
    lv_obj_set_style_bg_color(container, bg, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_center(container);
    return container;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_label_set_text(label, "--");
    return label;
}

static lv_obj_t *make_panel(lv_obj_t *parent,
                            int32_t w,
                            int32_t h,
                            int32_t radius,
                            lv_color_t color,
                            lv_opa_t opa)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_radius(panel, radius, 0);
    lv_obj_set_style_bg_color(panel, color, 0);
    lv_obj_set_style_bg_opa(panel, opa, 0);
    return panel;
}

static void replace_zero_with_o(char *text)
{
    for (char *p = text; *p != '\0'; ++p) {
        if (*p == '0') {
            *p = 'O';
        }
    }
}

static void set_obj_hidden(lv_obj_t *obj, bool hidden)
{
    if (obj == NULL) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *make_hand(lv_obj_t *parent,
                           const lv_image_dsc_t *src,
                           int32_t width,
                           int32_t height,
                           int32_t anchor_offset)
{
    lv_obj_t *hand = lv_image_create(parent);
    lv_image_set_src(hand, src);
    lv_obj_set_pos(hand, CENTER - anchor_offset, CENTER - height / 2);
    lv_image_set_pivot(hand, anchor_offset, height / 2);
    return hand;
}

static void set_hand_rotation(lv_obj_t *hand, double degrees)
{
    if (hand != NULL) {
        lv_image_set_rotation(hand, (int32_t)round(degrees * 10.0));
    }
}

static float clampf(float value, float min_value, float max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static float smootherstep(float t)
{
    t = clampf(t, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float unwrap_angle(float previous_unwrapped_angle, float next_angle)
{
    float previous_angle = fmodf(previous_unwrapped_angle, 360.0f);
    if (previous_angle < 0.0f) {
        previous_angle += 360.0f;
    }

    float delta = fmodf(next_angle - previous_angle, 360.0f);
    if (delta < -180.0f) {
        delta += 360.0f;
    } else if (delta > 180.0f) {
        delta -= 360.0f;
    }

    return previous_unwrapped_angle + delta;
}

static void set_img_recolor(lv_obj_t *image, uint32_t color)
{
    if (image == NULL) {
        return;
    }

    lv_obj_set_style_image_recolor(image, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
}

static const lv_image_dsc_t *get_big_number_image(int digit)
{
    switch (digit) {
    case 0:
        return &big_number_0;
    case 1:
        return &big_number_1;
    case 2:
        return &big_number_2;
    case 3:
        return &big_number_3;
    case 4:
        return &big_number_4;
    case 5:
        return &big_number_5;
    case 6:
        return &big_number_6;
    case 7:
        return &big_number_7;
    case 8:
        return &big_number_8;
    case 9:
    default:
        return &big_number_9;
    }
}

// M5 Classic
static lv_obj_t *s_m5_classic_time = NULL;
static lv_obj_t *s_m5_classic_weekday = NULL;
static lv_obj_t *s_m5_classic_date = NULL;
static lv_obj_t *s_m5_classic_hour = NULL;
static lv_obj_t *s_m5_classic_minute = NULL;
static lv_obj_t *s_m5_classic_second = NULL;
static lv_obj_t *s_m5_classic_center_cap = NULL;
static lv_obj_t *s_m5_classic_ticks = NULL;
static lv_obj_t *s_m5_classic_sub_panel = NULL;
static lv_obj_t *s_m5_classic_date_panel = NULL;
static int s_m5_classic_display_mode = 0;

static void make_classic_ticks(lv_obj_t *parent)
{
    constexpr int32_t tick_gap = 9;
    constexpr int32_t hour_tick_length = 28;
    constexpr int32_t minute_tick_length = 14;
    constexpr int32_t outer_radius = CENTER - tick_gap;

    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_draw_buf_t *draw_buf = lv_draw_buf_create(THEME_SIZE, THEME_SIZE, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (draw_buf == NULL) {
        return;
    }

    lv_canvas_set_draw_buf(canvas, draw_buf);
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);
    lv_obj_center(canvas);

    lv_obj_add_event_cb(canvas,
                        [](lv_event_t *event) {
                            lv_draw_buf_t *buf = (lv_draw_buf_t *)lv_event_get_user_data(event);
                            if (buf != NULL) {
                                lv_draw_buf_destroy(buf);
                            }
                        },
                        LV_EVENT_DELETE,
                        draw_buf);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    for (int i = 0; i < 60; ++i) {
        const bool hour_tick = (i % 5) == 0;
        const int32_t tick_length = hour_tick ? hour_tick_length : minute_tick_length;
        const int32_t inner_radius = outer_radius - tick_length;
        const float angle = (-90.0f + (float)i * 6.0f) * PI / 180.0f;

        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = lv_color_hex(hour_tick ? 0x6E6E6E : 0x454545);
        dsc.width = 2;
        dsc.p1.x = CENTER + lroundf((float)inner_radius * cosf(angle));
        dsc.p1.y = CENTER + lroundf((float)inner_radius * sinf(angle));
        dsc.p2.x = CENTER + lroundf((float)outer_radius * cosf(angle));
        dsc.p2.y = CENTER + lroundf((float)outer_radius * sinf(angle));
        lv_draw_line(&layer, &dsc);
    }

    lv_canvas_finish_layer(canvas, &layer);
    s_m5_classic_ticks = canvas;
}

static void apply_classic_display_mode(void)
{
    const bool show_info = s_m5_classic_display_mode == 0;
    const bool show_ticks = s_m5_classic_display_mode != 2;

    set_obj_hidden(s_m5_classic_sub_panel, !show_info);
    set_obj_hidden(s_m5_classic_time, !show_info);
    set_obj_hidden(s_m5_classic_weekday, !show_info);
    set_obj_hidden(s_m5_classic_date_panel, !show_info);
    set_obj_hidden(s_m5_classic_ticks, !show_ticks);
}

static void reset_classic_state(void)
{
    s_m5_classic_display_mode = 0;
    apply_classic_display_mode();
}

} // namespace

static lv_obj_t *m5_classic_build(lv_obj_t *parent)
{
    lv_obj_t *container = make_root(parent, lv_color_hex(0x000000));
    s_m5_classic_time = NULL;
    s_m5_classic_weekday = NULL;
    s_m5_classic_date = NULL;
    s_m5_classic_hour = NULL;
    s_m5_classic_minute = NULL;
    s_m5_classic_second = NULL;
    s_m5_classic_center_cap = NULL;
    s_m5_classic_ticks = NULL;
    s_m5_classic_sub_panel = NULL;
    s_m5_classic_date_panel = NULL;
    s_m5_classic_display_mode = 0;

    make_classic_ticks(container);

    s_m5_classic_sub_panel = make_panel(container, 358, 358, LV_RADIUS_CIRCLE, lv_color_hex(0x1B1B1B), LV_OPA_COVER);
    lv_obj_center(s_m5_classic_sub_panel);

    s_m5_classic_time = make_label(container, &lv_font_maple_mono_medium_28, lv_color_hex(0xD9D9D9));
    lv_obj_align(s_m5_classic_time, LV_ALIGN_CENTER, 0, 104);

    s_m5_classic_weekday = make_label(container, &lv_font_maple_mono_medium_24, lv_color_hex(0x717171));
    lv_obj_align(s_m5_classic_weekday, LV_ALIGN_CENTER, 0, 138);

    s_m5_classic_date_panel = make_panel(container, 48, 34, LV_RADIUS_CIRCLE, lv_color_hex(0x101010), LV_OPA_COVER);
    lv_obj_align(s_m5_classic_date_panel, LV_ALIGN_CENTER, 125, 0);

    s_m5_classic_date = make_label(s_m5_classic_date_panel, &lv_font_maple_mono_medium_24, lv_color_hex(0x8B8B8B));
    lv_obj_center(s_m5_classic_date);

    s_m5_classic_hour = make_hand(container, &classic_hour_hand, 126, 14, 18);
    s_m5_classic_minute = make_hand(container, &classic_minute_hand, 190, 8, 20);
    s_m5_classic_second = make_hand(container, &classic_second_hand, 240, 14, 29);

    s_m5_classic_center_cap = make_panel(container, 16, 16, LV_RADIUS_CIRCLE, lv_color_hex(0xE22B2B), LV_OPA_COVER);
    lv_obj_center(s_m5_classic_center_cap);

    apply_classic_display_mode();

    return container;
}

static void m5_classic_update(const struct tm *timeinfo)
{
    char time_text[8] = {};
    char date_text[4] = {};

    snprintf(time_text, sizeof(time_text), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
    snprintf(date_text, sizeof(date_text), "%02d", timeinfo->tm_mday);
    replace_zero_with_o(time_text);
    replace_zero_with_o(date_text);

    lv_label_set_text(s_m5_classic_time, time_text);
    lv_label_set_text(s_m5_classic_weekday, kWeekdayAbbr[timeinfo->tm_wday % 7]);
    lv_label_set_text(s_m5_classic_date, date_text);

    const double second_angle = (double)timeinfo->tm_sec * 6.0 - 90.0;
    const double minute_angle = ((double)timeinfo->tm_min + (double)timeinfo->tm_sec / 60.0) * 6.0 - 90.0;
    const double hour_angle = (((double)(timeinfo->tm_hour % 12) +
                                (double)timeinfo->tm_min / 60.0 +
                                (double)timeinfo->tm_sec / 3600.0) *
                               30.0) -
                              90.0;

    set_hand_rotation(s_m5_classic_hour, hour_angle);
    set_hand_rotation(s_m5_classic_minute, minute_angle);
    set_hand_rotation(s_m5_classic_second, second_angle);
}

// M5 Simple
namespace {

static constexpr int SIMPLE_TICK_COUNT = 12;
static constexpr float SIMPLE_DOT_RADIUS = 214.0f;
static constexpr float SIMPLE_TICK_RADIUS = 194.0f;

static lv_obj_t *s_m5_simple_time = NULL;
static lv_obj_t *s_m5_simple_date = NULL;
static lv_obj_t *s_m5_simple_dot = NULL;
static lv_obj_t *s_m5_simple_ring = NULL;
static lv_obj_t *s_m5_simple_ticks[SIMPLE_TICK_COUNT] = {};
static lv_obj_t *s_m5_simple_root = NULL;
static bool s_m5_simple_angle_initialized = false;
static float s_m5_simple_angle_unwrapped = 0.0f;
static float s_m5_simple_anim_start = 0.0f;
static float s_m5_simple_anim_target = 0.0f;
static uint32_t s_m5_simple_anim_start_tick = 0;
static bool s_m5_simple_anim_active = false;

static void apply_simple_dot(float angle_deg)
{
    if (s_m5_simple_dot == NULL) {
        return;
    }

    const float radians = (angle_deg - 90.0f) * PI / 180.0f;
    const int32_t x = lroundf(SIMPLE_DOT_RADIUS * cosf(radians));
    const int32_t y = lroundf(SIMPLE_DOT_RADIUS * sinf(radians));
    lv_obj_align(s_m5_simple_dot, LV_ALIGN_CENTER, x, y);
}

static lv_obj_t *make_simple_ring(lv_obj_t *parent)
{
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ring, 430, 430);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_20, 0);
    lv_obj_center(ring);
    return ring;
}

static lv_obj_t *make_simple_tick(lv_obj_t *parent, int index)
{
    const bool major = (index % 3) == 0;
    const int32_t size = major ? 7 : 4;
    const float radians = ((float)index * 30.0f - 90.0f) * PI / 180.0f;
    const int32_t x = lroundf(SIMPLE_TICK_RADIUS * cosf(radians));
    const int32_t y = lroundf(SIMPLE_TICK_RADIUS * sinf(radians));

    lv_obj_t *tick = make_panel(parent, size, size, LV_RADIUS_CIRCLE, lv_color_hex(0xA0A0A0),
                                major ? LV_OPA_50 : LV_OPA_30);
    lv_obj_align(tick, LV_ALIGN_CENTER, x, y);
    return tick;
}

static void set_simple_target_angle(float angle_deg)
{
    if (!s_m5_simple_angle_initialized) {
        s_m5_simple_angle_initialized = true;
        s_m5_simple_angle_unwrapped = angle_deg;
        s_m5_simple_anim_start = angle_deg;
        s_m5_simple_anim_target = angle_deg;
        s_m5_simple_anim_active = false;
        apply_simple_dot(angle_deg);
        return;
    }

    const float next_unwrapped = unwrap_angle(s_m5_simple_angle_unwrapped, angle_deg);
    s_m5_simple_angle_unwrapped = next_unwrapped;

    const uint32_t now = lv_tick_get();
    float current = s_m5_simple_anim_target;
    if (s_m5_simple_anim_active) {
        const float progress = clampf((float)(now - s_m5_simple_anim_start_tick) / 300.0f, 0.0f, 1.0f);
        current = s_m5_simple_anim_start + (s_m5_simple_anim_target - s_m5_simple_anim_start) * smootherstep(progress);
    }

    s_m5_simple_anim_start = current;
    s_m5_simple_anim_target = next_unwrapped;
    s_m5_simple_anim_start_tick = now;
    s_m5_simple_anim_active = true;
}

static bool tick_simple(uint32_t now_ms)
{
    bool changed = false;

    if (s_m5_simple_anim_active) {
        const float progress = clampf((float)(now_ms - s_m5_simple_anim_start_tick) / 300.0f, 0.0f, 1.0f);
        const float angle = s_m5_simple_anim_start +
                            (s_m5_simple_anim_target - s_m5_simple_anim_start) * smootherstep(progress);
        apply_simple_dot(angle);
        if (progress >= 1.0f) {
            s_m5_simple_anim_active = false;
        }
        changed = true;
    }

    return changed;
}

static void apply_simple_theme(int theme_index)
{
    const M5Theme &theme = kSimpleThemes[theme_index];
    if (s_m5_simple_root != NULL) {
        lv_obj_set_style_bg_color(s_m5_simple_root, lv_color_hex(theme.background_color), 0);
    }
    if (s_m5_simple_time != NULL) {
        lv_obj_set_style_text_color(s_m5_simple_time, lv_color_hex(theme.time_color), 0);
    }
    if (s_m5_simple_date != NULL) {
        lv_obj_set_style_text_color(s_m5_simple_date, lv_color_hex(theme.date_color), 0);
    }
    if (s_m5_simple_ring != NULL) {
        lv_obj_set_style_border_color(s_m5_simple_ring, lv_color_hex(theme.accent_color), 0);
    }
    for (int i = 0; i < SIMPLE_TICK_COUNT; ++i) {
        if (s_m5_simple_ticks[i] != NULL) {
            lv_obj_set_style_bg_color(s_m5_simple_ticks[i], lv_color_hex(theme.accent_color), 0);
        }
    }
    if (s_m5_simple_dot != NULL) {
        lv_obj_set_style_bg_color(s_m5_simple_dot, lv_color_hex(theme.accent_color), 0);
    }
}

static void reset_simple_state(void)
{
    s_m5_simple_angle_initialized = false;
    s_m5_simple_angle_unwrapped = 0.0f;
    s_m5_simple_anim_start = 0.0f;
    s_m5_simple_anim_target = 0.0f;
    s_m5_simple_anim_start_tick = lv_tick_get();
    s_m5_simple_anim_active = false;
    apply_simple_theme(0);
}

} // namespace

static lv_obj_t *m5_simple_build(lv_obj_t *parent)
{
    lv_obj_t *container = make_root(parent, lv_color_hex(0x000000));
    s_m5_simple_root = container;
    s_m5_simple_time = NULL;
    s_m5_simple_date = NULL;
    s_m5_simple_dot = NULL;
    s_m5_simple_ring = NULL;
    memset(s_m5_simple_ticks, 0, sizeof(s_m5_simple_ticks));

    s_m5_simple_ring = make_simple_ring(container);
    for (int i = 0; i < SIMPLE_TICK_COUNT; ++i) {
        s_m5_simple_ticks[i] = make_simple_tick(container, i);
    }

    s_m5_simple_time = make_label(container, &CommissionerMedium108, lv_color_hex(0xFFFFFF));
    lv_obj_align(s_m5_simple_time, LV_ALIGN_CENTER, 0, -28);

    s_m5_simple_date = make_label(container, &lv_font_montserrat_24, lv_color_hex(0xA0A0A0));
    lv_obj_align(s_m5_simple_date, LV_ALIGN_CENTER, 0, 70);

    s_m5_simple_dot = make_panel(container, 14, 14, LV_RADIUS_CIRCLE, lv_color_hex(0xA0A0A0), LV_OPA_COVER);
    apply_simple_dot(0.0f);

    reset_simple_state();

    return container;
}

static void m5_simple_update(const struct tm *timeinfo)
{
    char time_text[8] = {};
    char date_text[32] = {};

    snprintf(time_text, sizeof(time_text), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
    snprintf(date_text, sizeof(date_text), "%d/%d/%d %s",
             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
             kWeekdayAbbr[timeinfo->tm_wday % 7]);

    lv_label_set_text(s_m5_simple_time, time_text);
    lv_label_set_text(s_m5_simple_date, date_text);
    set_simple_target_angle((float)(timeinfo->tm_sec * 6));
}

// M5 BigNumber
namespace {

static lv_obj_t *s_m5_big_digits[4] = {};

static lv_obj_t *make_big_digit(lv_obj_t *parent, int32_t x, int32_t y, uint32_t color)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_obj_align(img, LV_ALIGN_CENTER, x, y);
    set_img_recolor(img, color);
    return img;
}

static void apply_big_number_palette(int palette_index)
{
    for (int i = 0; i < 4; ++i) {
        set_img_recolor(s_m5_big_digits[i], kBigNumberPalettes[palette_index][i]);
    }
}

} // namespace

static lv_obj_t *m5_big_number_build(lv_obj_t *parent)
{
    lv_obj_t *container = make_root(parent, lv_color_hex(0x000000));
    memset(s_m5_big_digits, 0, sizeof(s_m5_big_digits));

    s_m5_big_digits[0] = make_big_digit(container, -42, -51, 0xCBDB8C);
    s_m5_big_digits[1] = make_big_digit(container, 42, -51, 0xD3EA71);
    s_m5_big_digits[2] = make_big_digit(container, -42, 51, 0xE9F5B8);
    s_m5_big_digits[3] = make_big_digit(container, 42, 51, 0x94A350);
    apply_big_number_palette(0);

    return container;
}

static void m5_big_number_update(const struct tm *timeinfo)
{
    const int digits[4] = {
        timeinfo->tm_hour / 10,
        timeinfo->tm_hour % 10,
        timeinfo->tm_min / 10,
        timeinfo->tm_min % 10,
    };

    for (int i = 0; i < 4; ++i) {
        lv_image_set_src(s_m5_big_digits[i], get_big_number_image(digits[i]));
    }
}

// M5 NumberFlow, static LVGL equivalent layout.
namespace {

static lv_obj_t *s_m5_flow_hour = NULL;
static lv_obj_t *s_m5_flow_minute = NULL;
static lv_obj_t *s_m5_flow_second = NULL;
static lv_obj_t *s_m5_flow_date = NULL;
static lv_obj_t *s_m5_flow_root = NULL;
static lv_obj_t *s_m5_flow_panels[3] = {};

struct FlowDigits {
    lv_obj_t *clip = NULL;
    lv_obj_t *current = NULL;
    lv_obj_t *next = NULL;
    int value = -1;
    int previous_value = -1;
    uint32_t start_tick = 0;
    bool animating = false;
};

static FlowDigits s_m5_flow_digits[3] = {};

static lv_obj_t *make_flow_panel(lv_obj_t *parent, int32_t x)
{
    lv_obj_t *panel = make_panel(parent, 100, 132, 42, lv_color_hex(0x1A1919), LV_OPA_COVER);
    lv_obj_align(panel, LV_ALIGN_CENTER, x, -22);
    return panel;
}

static lv_obj_t *make_flow_digit_label(lv_obj_t *parent)
{
    lv_obj_t *label = make_label(parent, &CommissionerMedium64, lv_color_hex(0xFFFFFF));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    return label;
}

static void set_flow_label_value(lv_obj_t *label, int value)
{
    char text[8] = {};
    snprintf(text, sizeof(text), "%02d", value);
    lv_label_set_text(label, text);
}

static lv_obj_t *make_flow_digit(lv_obj_t *panel, FlowDigits *flow)
{
    lv_obj_t *clip = lv_obj_create(panel);
    lv_obj_remove_style_all(clip);
    lv_obj_clear_flag(clip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(clip, 100, 132);
    lv_obj_center(clip);
    lv_obj_set_style_clip_corner(clip, true, 0);

    flow->clip = clip;
    flow->current = make_flow_digit_label(clip);
    flow->next = make_flow_digit_label(clip);
    lv_obj_add_flag(flow->next, LV_OBJ_FLAG_HIDDEN);
    return clip;
}

static void set_flow_digit_color(FlowDigits *flow, uint32_t color)
{
    if (flow->current != NULL) {
        lv_obj_set_style_text_color(flow->current, lv_color_hex(color), 0);
    }
    if (flow->next != NULL) {
        lv_obj_set_style_text_color(flow->next, lv_color_hex(color), 0);
    }
}

static void set_flow_value(FlowDigits *flow, int value)
{
    if (flow->value < 0) {
        flow->value = value;
        flow->previous_value = value;
        set_flow_label_value(flow->current, value);
        lv_obj_center(flow->current);
        return;
    }

    if (flow->value == value) {
        return;
    }

    flow->previous_value = flow->value;
    flow->value = value;
    flow->start_tick = lv_tick_get();
    flow->animating = true;

    set_flow_label_value(flow->current, flow->previous_value);
    set_flow_label_value(flow->next, flow->value);
    lv_obj_clear_flag(flow->next, LV_OBJ_FLAG_HIDDEN);
}

static bool tick_flow_digit(FlowDigits *flow, uint32_t now_ms)
{
    if (!flow->animating) {
        return false;
    }

    const float progress = clampf((float)(now_ms - flow->start_tick) / 420.0f, 0.0f, 1.0f);
    const float eased = smootherstep(progress);
    const int32_t current_y = lroundf(-132.0f * eased);
    const int32_t next_y = lroundf(132.0f * (1.0f - eased));

    lv_obj_align(flow->current, LV_ALIGN_CENTER, 0, current_y);
    lv_obj_align(flow->next, LV_ALIGN_CENTER, 0, next_y);

    if (progress >= 1.0f) {
        lv_label_set_text(flow->current, lv_label_get_text(flow->next));
        lv_obj_center(flow->current);
        lv_obj_add_flag(flow->next, LV_OBJ_FLAG_HIDDEN);
        flow->animating = false;
    }

    return true;
}

static void apply_number_flow_theme(int theme_index)
{
    const M5FlowTheme &theme = kNumberFlowThemes[theme_index];
    if (s_m5_flow_root != NULL) {
        lv_obj_set_style_bg_color(s_m5_flow_root, lv_color_hex(theme.background_color), 0);
    }
    for (int i = 0; i < 3; ++i) {
        if (s_m5_flow_panels[i] != NULL) {
            lv_obj_set_style_bg_color(s_m5_flow_panels[i], lv_color_hex(theme.panel_color), 0);
        }
        set_flow_digit_color(&s_m5_flow_digits[i], theme.time_color);
    }
    if (s_m5_flow_date != NULL) {
        lv_obj_set_style_text_color(s_m5_flow_date, lv_color_hex(theme.date_color), 0);
    }
}

static void reset_number_flow_state(void)
{
    for (int i = 0; i < 3; ++i) {
        s_m5_flow_digits[i].value = -1;
        s_m5_flow_digits[i].previous_value = -1;
        s_m5_flow_digits[i].animating = false;
    }
    apply_number_flow_theme(0);
}

} // namespace

static lv_obj_t *m5_number_flow_build(lv_obj_t *parent)
{
    lv_obj_t *container = make_root(parent, lv_color_hex(0x000000));
    s_m5_flow_root = container;
    s_m5_flow_hour = NULL;
    s_m5_flow_minute = NULL;
    s_m5_flow_second = NULL;
    s_m5_flow_date = NULL;
    memset(s_m5_flow_panels, 0, sizeof(s_m5_flow_panels));
    memset(s_m5_flow_digits, 0, sizeof(s_m5_flow_digits));

    s_m5_flow_panels[0] = make_flow_panel(container, -117);
    s_m5_flow_panels[1] = make_flow_panel(container, 0);
    s_m5_flow_panels[2] = make_flow_panel(container, 117);

    s_m5_flow_hour = make_flow_digit(s_m5_flow_panels[0], &s_m5_flow_digits[0]);
    s_m5_flow_minute = make_flow_digit(s_m5_flow_panels[1], &s_m5_flow_digits[1]);
    s_m5_flow_second = make_flow_digit(s_m5_flow_panels[2], &s_m5_flow_digits[2]);

    s_m5_flow_date = make_label(container, &lv_font_montserrat_24, lv_color_hex(0xA0A0A0));
    lv_obj_align(s_m5_flow_date, LV_ALIGN_CENTER, 0, 76);

    reset_number_flow_state();

    return container;
}

static void m5_number_flow_update(const struct tm *timeinfo)
{
    char text[32] = {};

    set_flow_value(&s_m5_flow_digits[0], timeinfo->tm_hour);
    set_flow_value(&s_m5_flow_digits[1], timeinfo->tm_min);
    set_flow_value(&s_m5_flow_digits[2], timeinfo->tm_sec);

    snprintf(text, sizeof(text), "%d/%d/%d %s",
             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
             kWeekdayAbbr[timeinfo->tm_wday % 7]);
    lv_label_set_text(s_m5_flow_date, text);
}

static bool m5_number_flow_tick(uint32_t now_ms)
{
    bool changed = false;
    for (int i = 0; i < 3; ++i) {
        changed = tick_flow_digit(&s_m5_flow_digits[i], now_ms) || changed;
    }
    return changed;
}

const clock_theme_t g_clock_theme_m5_classic = {
    .name = "m5_classic",
    .build = m5_classic_build,
    .update = m5_classic_update,
    .reset = reset_classic_state,
};

const clock_theme_t g_clock_theme_m5_simple = {
    .name = "m5_simple",
    .build = m5_simple_build,
    .update = m5_simple_update,
    .reset = reset_simple_state,
    .tick = tick_simple,
};

const clock_theme_t g_clock_theme_m5_big_number = {
    .name = "m5_big_number",
    .build = m5_big_number_build,
    .update = m5_big_number_update,
};

const clock_theme_t g_clock_theme_m5_number_flow = {
    .name = "m5_number_flow",
    .build = m5_number_flow_build,
    .update = m5_number_flow_update,
    .reset = reset_number_flow_state,
    .tick = m5_number_flow_tick,
};
