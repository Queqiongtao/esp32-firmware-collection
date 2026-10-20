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

static constexpr int SIMPLE_TICK_COUNT = 12;
static constexpr float SIMPLE_DOT_RADIUS = 214.0f;
static constexpr float SIMPLE_TICK_RADIUS = 194.0f;
static constexpr int32_t SIMPLE_DOT_SIZE = 14;

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
static float s_m5_simple_dot_offset_x = 0.0f;
static float s_m5_simple_dot_offset_y = 0.0f;
static float s_m5_simple_rotation_deg = 0.0f;

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

static void position_simple_dot(void)
{
    if (s_m5_simple_dot == NULL) {
        return;
    }

    const float radians = s_m5_simple_rotation_deg * PI / 180.0f;
    const float cos_a = cosf(radians);
    const float sin_a = sinf(radians);
    const int32_t x = lroundf(cos_a * s_m5_simple_dot_offset_x -
                             sin_a * s_m5_simple_dot_offset_y);
    const int32_t y = lroundf(sin_a * s_m5_simple_dot_offset_x +
                             cos_a * s_m5_simple_dot_offset_y);

    lv_obj_set_pos(s_m5_simple_dot,
                   CENTER + x - SIMPLE_DOT_SIZE / 2,
                   CENTER + y - SIMPLE_DOT_SIZE / 2);
}

static void apply_simple_dot(float angle_deg)
{
    if (s_m5_simple_dot == NULL) {
        return;
    }

    const float radians = (angle_deg - 90.0f) * PI / 180.0f;
    s_m5_simple_dot_offset_x = SIMPLE_DOT_RADIUS * cosf(radians);
    s_m5_simple_dot_offset_y = SIMPLE_DOT_RADIUS * sinf(radians);
    position_simple_dot();
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

static void destroy_simple_overlay(void)
{
    if (s_m5_simple_dot != NULL) {
        lv_obj_del(s_m5_simple_dot);
        s_m5_simple_dot = NULL;
    }
}

static void set_simple_overlay_visible(bool visible)
{
    set_obj_hidden(s_m5_simple_dot, !visible);
}

static void set_simple_rotation(float angle_deg)
{
    s_m5_simple_rotation_deg = angle_deg;
    position_simple_dot();
}

static bool simple_tick_updates_overlay_only(void)
{
    return true;
}

} // namespace

static lv_obj_t *m5_simple_build(lv_obj_t *parent)
{
    destroy_simple_overlay();
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

    s_m5_simple_dot =
        make_panel(parent, SIMPLE_DOT_SIZE, SIMPLE_DOT_SIZE, LV_RADIUS_CIRCLE,
                   lv_color_hex(0xA0A0A0), LV_OPA_COVER);
    apply_simple_dot(0.0f);

    reset_simple_state();

    return container;
}

static bool m5_simple_update(const struct tm *timeinfo)
{
    char time_text[8] = {};
    char date_text[32] = {};

    snprintf(time_text, sizeof(time_text), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
    snprintf(date_text, sizeof(date_text), "%d/%d/%d %s",
             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
             kWeekdayAbbr[timeinfo->tm_wday % 7]);

    const bool time_changed = strcmp(lv_label_get_text(s_m5_simple_time), time_text) != 0;
    const bool date_changed = strcmp(lv_label_get_text(s_m5_simple_date), date_text) != 0;
    if (time_changed) {
        lv_label_set_text(s_m5_simple_time, time_text);
    }
    if (date_changed) {
        lv_label_set_text(s_m5_simple_date, date_text);
    }
    set_simple_target_angle((float)(timeinfo->tm_sec * 6));
    return time_changed || date_changed;
}

const clock_theme_t g_clock_theme_m5_simple = {
    .name = "m5_simple",
    .build = m5_simple_build,
    .update = m5_simple_update,
    .reset = reset_simple_state,
    .destroy_overlay = destroy_simple_overlay,
    .tick = tick_simple,
    .tick_updates_overlay_only = simple_tick_updates_overlay_only,
    .set_overlay_visible = set_simple_overlay_visible,
    .set_rotation = set_simple_rotation,
};
