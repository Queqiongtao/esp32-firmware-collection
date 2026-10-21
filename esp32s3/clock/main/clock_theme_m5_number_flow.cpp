#include "clock_theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "m5_watch_assets.h"

namespace {

constexpr int32_t THEME_SIZE = 466;
constexpr int32_t FLOW_PANEL_WIDTH = 120;
constexpr int32_t FLOW_PANEL_HEIGHT = 152;
constexpr int32_t FLOW_PANEL_RADIUS = 51;
constexpr int32_t FLOW_PANEL_X = 135;
constexpr int32_t FLOW_PANEL_Y = -22;
constexpr int32_t FLOW_LABEL_Y = -116;
constexpr int32_t FLOW_DATE_Y = 82;
constexpr uint32_t FLOW_ANIMATION_MS = 550;
constexpr uint32_t FLOW_PANEL_COLOR = 0x222222;
constexpr uint32_t FLOW_MUTED_COLOR = 0x8FA6AD;
constexpr uint32_t FLOW_SECOND_COLOR = 0xFF7A59;

static const char *const kWeekdayAbbr[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

struct M5FlowTheme {
    uint32_t background_color;
    uint32_t panel_color;
    uint32_t time_color;
    uint32_t accent_color; // 秒位数字和 S 标签
    uint32_t date_color;
};

static const M5FlowTheme kNumberFlowThemes[10] = {
    {0x000000, FLOW_PANEL_COLOR, 0xFFFFFF, FLOW_SECOND_COLOR, FLOW_MUTED_COLOR},
    {0xEEEEEE, 0xE4E3E3, 0x484848, 0x484848, 0x525252},
    {0xC2EFEB, 0xB1E1DD, 0x566E6C, 0x566E6C, 0x6A938F},
    {0xEEE0CB, 0xD9CAB2, 0x6A5E4D, 0x6A5E4D, 0x948063},
    {0xB1CC74, 0xA4BD6B, 0x4B5E20, 0x4B5E20, 0x668323},
    {0x8A89C0, 0x8180B4, 0xDFDEFF, 0xDFDEFF, 0xCBCBFF},
    {0xF26419, 0xF6722C, 0xFFEFE6, 0xFFEFE6, 0xFFD3BC},
    {0xF2D7EE, 0xE9C9E5, 0x83627E, 0x83627E, 0xA4789D},
    {0x048A81, 0x05968C, 0xB1FFFA, 0xB1FFFA, 0x70CFC9},
    {0x9DB4C0, 0x92A8B3, 0xECF8FF, 0xECF8FF, 0xDCF3FF},
};

struct FlowDigit {
    lv_obj_t *clip = NULL;
    lv_obj_t *current = NULL;
    lv_obj_t *next = NULL;
    int value = -1;
    int previous_value = -1;
    uint32_t start_tick = 0;
    bool animating = false;
};

static lv_obj_t *s_m5_flow_date = NULL;
static lv_obj_t *s_m5_flow_root = NULL;
static lv_obj_t *s_m5_flow_panels[3] = {};
static lv_obj_t *s_m5_flow_labels[3] = {};
static FlowDigit s_m5_flow_digits[3][2] = {};
static int32_t s_m5_flow_roll_distance = 0;

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

static float ease_out_cubic(float t)
{
    t = clampf(t, 0.0f, 1.0f);
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

static lv_obj_t *make_flow_panel(lv_obj_t *parent, int32_t x)
{
    lv_obj_t *panel = make_panel(parent, FLOW_PANEL_WIDTH, FLOW_PANEL_HEIGHT,
                                 FLOW_PANEL_RADIUS, lv_color_hex(FLOW_PANEL_COLOR), LV_OPA_COVER);
    lv_obj_align(panel, LV_ALIGN_CENTER, x, FLOW_PANEL_Y);
    return panel;
}

static lv_obj_t *make_flow_digit_label(lv_obj_t *parent)
{
    lv_obj_t *label = make_label(parent, &CommissionerMedium80, lv_color_hex(0xFFFFFF));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, "0");
    lv_obj_center(label);
    return label;
}

static void set_flow_label_value(lv_obj_t *label, int value)
{
    char text[12] = {};
    snprintf(text, sizeof(text), "%d", value);
    lv_label_set_text(label, text);
}

static void make_flow_digit(lv_obj_t *panel,
                            FlowDigit *flow,
                            int32_t x,
                            int32_t width,
                            int32_t height)
{
    lv_obj_t *clip = lv_obj_create(panel);
    lv_obj_remove_style_all(clip);
    lv_obj_clear_flag(clip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(clip, width, height);
    lv_obj_align(clip, LV_ALIGN_CENTER, x, 0);

    flow->clip = clip;
    flow->current = make_flow_digit_label(clip);
    flow->next = make_flow_digit_label(clip);
    lv_obj_add_flag(flow->next, LV_OBJ_FLAG_HIDDEN);
}

static void set_flow_digit_color(FlowDigit *flow, uint32_t color)
{
    if (flow->current != NULL) {
        lv_obj_set_style_text_color(flow->current, lv_color_hex(color), 0);
    }
    if (flow->next != NULL) {
        lv_obj_set_style_text_color(flow->next, lv_color_hex(color), 0);
    }
}

static void snap_flow_digit(FlowDigit *flow)
{
    if (!flow->animating) {
        return;
    }

    lv_label_set_text(flow->current, lv_label_get_text(flow->next));
    lv_obj_center(flow->current);
    lv_obj_add_flag(flow->next, LV_OBJ_FLAG_HIDDEN);
    flow->animating = false;
}

static bool set_flow_value(FlowDigit *flow, int value)
{
    if (flow->value < 0) {
        flow->value = value;
        flow->previous_value = value;
        set_flow_label_value(flow->current, value);
        lv_obj_center(flow->current);
        lv_obj_add_flag(flow->next, LV_OBJ_FLAG_HIDDEN);
        return true;
    }

    if (flow->value == value) {
        return false;
    }

    snap_flow_digit(flow);

    flow->previous_value = flow->value;
    flow->value = value;
    flow->start_tick = lv_tick_get();
    flow->animating = true;

    set_flow_label_value(flow->current, flow->previous_value);
    lv_obj_align(flow->current, LV_ALIGN_CENTER, 0, 0);
    set_flow_label_value(flow->next, flow->value);
    lv_obj_align(flow->next, LV_ALIGN_CENTER, 0, s_m5_flow_roll_distance);
    lv_obj_clear_flag(flow->next, LV_OBJ_FLAG_HIDDEN);
    return true;
}

static bool tick_flow_digit(FlowDigit *flow, uint32_t now_ms)
{
    if (!flow->animating) {
        return false;
    }

    const float progress = clampf(
        (float)(now_ms - flow->start_tick) / (float)FLOW_ANIMATION_MS, 0.0f, 1.0f);
    const float eased = ease_out_cubic(progress);
    const int32_t current_y = lroundf(-(float)s_m5_flow_roll_distance * eased);
    const int32_t next_y = lroundf((float)s_m5_flow_roll_distance * (1.0f - eased));

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
        const uint32_t digit_color = i == 2 ? theme.accent_color : theme.time_color;
        for (int d = 0; d < 2; ++d) {
            set_flow_digit_color(&s_m5_flow_digits[i][d], digit_color);
        }
        if (s_m5_flow_labels[i] != NULL) {
            const uint32_t label_color = i == 2 ? theme.accent_color : theme.date_color;
            lv_obj_set_style_text_color(s_m5_flow_labels[i], lv_color_hex(label_color), 0);
        }
    }
    if (s_m5_flow_date != NULL) {
        lv_obj_set_style_text_color(s_m5_flow_date, lv_color_hex(theme.date_color), 0);
    }
}

static void reset_number_flow_state(void)
{
    for (int i = 0; i < 3; ++i) {
        for (int d = 0; d < 2; ++d) {
            FlowDigit &flow = s_m5_flow_digits[i][d];
            flow.value = -1;
            flow.previous_value = -1;
            flow.animating = false;
            if (flow.next != NULL) {
                lv_obj_add_flag(flow.next, LV_OBJ_FLAG_HIDDEN);
            }
            if (flow.current != NULL) {
                lv_obj_center(flow.current);
            }
        }
    }
    apply_number_flow_theme(0);
}

static bool update_flow_panel(int panel_index, int value)
{
    const int tens = value / 10;
    const int ones = value % 10;
    bool changed = set_flow_value(&s_m5_flow_digits[panel_index][0], tens);
    changed = set_flow_value(&s_m5_flow_digits[panel_index][1], ones) || changed;
    return changed;
}

} // namespace

static lv_obj_t *m5_number_flow_build(lv_obj_t *parent)
{
    lv_obj_t *container = make_root(parent, lv_color_hex(0x000000));
    s_m5_flow_root = container;
    s_m5_flow_date = NULL;
    memset(s_m5_flow_panels, 0, sizeof(s_m5_flow_panels));
    memset(s_m5_flow_labels, 0, sizeof(s_m5_flow_labels));
    memset(s_m5_flow_digits, 0, sizeof(s_m5_flow_digits));

    s_m5_flow_roll_distance = lv_font_get_line_height(&CommissionerMedium80);
    const int32_t digit_width = lv_font_get_glyph_width(&CommissionerMedium80, '0', '0');
    const int32_t clip_width = digit_width + 4;
    const int32_t left_x = -(digit_width / 2);
    const int32_t right_x = digit_width + left_x;

    for (int i = 0; i < 3; ++i) {
        const int32_t panel_x = -FLOW_PANEL_X + i * FLOW_PANEL_X;
        s_m5_flow_panels[i] = make_flow_panel(container, panel_x);
        make_flow_digit(s_m5_flow_panels[i], &s_m5_flow_digits[i][0],
                        left_x, clip_width, s_m5_flow_roll_distance);
        make_flow_digit(s_m5_flow_panels[i], &s_m5_flow_digits[i][1],
                        right_x, clip_width, s_m5_flow_roll_distance);
    }

    static const char *const kFlowLabels[3] = {"H", "M", "S"};
    for (int i = 0; i < 3; ++i) {
        s_m5_flow_labels[i] = make_label(container, &lv_font_montserrat_16,
                                         lv_color_hex(FLOW_MUTED_COLOR));
        lv_obj_set_style_text_opa(s_m5_flow_labels[i], LV_OPA_COVER, 0);
        lv_label_set_text(s_m5_flow_labels[i], kFlowLabels[i]);
        lv_obj_align(s_m5_flow_labels[i], LV_ALIGN_CENTER,
                     -FLOW_PANEL_X + i * FLOW_PANEL_X, FLOW_LABEL_Y);
    }

    s_m5_flow_date = make_label(container, &lv_font_montserrat_28,
                                lv_color_hex(FLOW_MUTED_COLOR));
    lv_obj_align(s_m5_flow_date, LV_ALIGN_CENTER, 0, FLOW_DATE_Y);

    reset_number_flow_state();

    return container;
}

static bool m5_number_flow_update(const struct tm *timeinfo)
{
    char text[32] = {};

    bool changed = update_flow_panel(0, timeinfo->tm_hour);
    changed = update_flow_panel(1, timeinfo->tm_min) || changed;
    changed = update_flow_panel(2, timeinfo->tm_sec) || changed;

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
        for (int d = 0; d < 2; ++d) {
            changed = tick_flow_digit(&s_m5_flow_digits[i][d], now_ms) || changed;
        }
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
