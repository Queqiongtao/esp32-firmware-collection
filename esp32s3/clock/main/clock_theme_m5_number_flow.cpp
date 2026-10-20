#include "clock_theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "m5_watch_assets.h"

namespace {

constexpr int32_t THEME_SIZE = 466;

static const char *const kWeekdayAbbr[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

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

static bool set_flow_value(FlowDigits *flow, int value)
{
    if (flow->value < 0) {
        flow->value = value;
        flow->previous_value = value;
        set_flow_label_value(flow->current, value);
        lv_obj_center(flow->current);
        return true;
    }

    if (flow->value == value) {
        return false;
    }

    flow->previous_value = flow->value;
    flow->value = value;
    flow->start_tick = lv_tick_get();
    flow->animating = true;

    set_flow_label_value(flow->current, flow->previous_value);
    set_flow_label_value(flow->next, flow->value);
    lv_obj_clear_flag(flow->next, LV_OBJ_FLAG_HIDDEN);
    return true;
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

static bool m5_number_flow_update(const struct tm *timeinfo)
{
    char text[32] = {};

    bool changed = set_flow_value(&s_m5_flow_digits[0], timeinfo->tm_hour);
    changed = set_flow_value(&s_m5_flow_digits[1], timeinfo->tm_min) || changed;
    changed = set_flow_value(&s_m5_flow_digits[2], timeinfo->tm_sec) || changed;

    snprintf(text, sizeof(text), "%d/%d/%d %s",
             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
             kWeekdayAbbr[timeinfo->tm_wday % 7]);
    const bool date_changed = strcmp(lv_label_get_text(s_m5_flow_date), text) != 0;
    if (date_changed) {
        lv_label_set_text(s_m5_flow_date, text);
    }
    return changed || date_changed;
}

static bool m5_number_flow_tick(uint32_t now_ms)
{
    bool changed = false;
    for (int i = 0; i < 3; ++i) {
        changed = tick_flow_digit(&s_m5_flow_digits[i], now_ms) || changed;
    }
    return changed;
}

const clock_theme_t g_clock_theme_m5_number_flow = {
    .name = "m5_number_flow",
    .build = m5_number_flow_build,
    .update = m5_number_flow_update,
    .reset = reset_number_flow_state,
    .tick = m5_number_flow_tick,
};
