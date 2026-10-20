#include "clock_theme.h"

#include <math.h>
#include <stdio.h>

#include "lvgl.h"
#include "m5_watch_assets.h"

namespace {

constexpr int32_t THEME_SIZE = 466;
constexpr int32_t CENTER = 233;
constexpr float PI = 3.14159265358979323846f;

static const char *const kWeekdayAbbr[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

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

static bool m5_classic_update(const struct tm *timeinfo)
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
    return true;
}

const clock_theme_t g_clock_theme_m5_classic = {
    .name = "m5_classic",
    .build = m5_classic_build,
    .update = m5_classic_update,
    .reset = reset_classic_state,
};
