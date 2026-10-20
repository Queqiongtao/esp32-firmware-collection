#include "clock_theme.h"

#include <stdio.h>

#include "lvgl.h"

extern "C" const lv_font_t lv_font_seg_96;
extern "C" const lv_font_t lv_font_seg_36;

namespace {

constexpr int32_t THEME_SIZE = 466;

#define SEGMENT_BG_COLOR lv_color_hex(0x000000)
#define SEGMENT_TIME_COLOR lv_color_hex(0xFFB000)
#define SEGMENT_ACCENT_COLOR lv_color_hex(0xFF5C35)
#define SEGMENT_DATE_COLOR lv_color_hex(0xFFE7C2)
#define SEGMENT_TITLE_COLOR lv_color_hex(0xB87800)
#define SEGMENT_PANEL_COLOR lv_color_hex(0x160E05)
#define SEGMENT_PANEL_BORDER_COLOR lv_color_hex(0x5A360C)
#define SEGMENT_TRACK_COLOR lv_color_hex(0x3A1A0A)

constexpr int32_t PANEL_X = 64;
constexpr int32_t PANEL_Y = 130;
constexpr int32_t PANEL_WIDTH = 338;
constexpr int32_t PANEL_HEIGHT = 116;
constexpr int32_t PROGRESS_WIDTH = 338;
constexpr int32_t PROGRESS_HEIGHT = 6;

static lv_obj_t *s_time_fg = NULL;
static lv_obj_t *s_seconds_fg = NULL;
static lv_obj_t *s_date_fg = NULL;
static lv_obj_t *s_progress_fill = NULL;

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

static lv_obj_t *make_label(lv_obj_t *parent,
                            const char *text,
                            const lv_font_t *font,
                            lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, text);
    return label;
}

} // namespace

static lv_obj_t *segment_build(lv_obj_t *parent)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(container, THEME_SIZE, THEME_SIZE);
    lv_obj_set_style_bg_color(container, SEGMENT_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_center(container);

    lv_obj_t *title_dot = make_box(container, 7, 7, SEGMENT_ACCENT_COLOR, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_align(title_dot, LV_ALIGN_TOP_LEFT, 64, 94);

    lv_obj_t *title = make_label(container, "SEGMENT", &lv_font_montserrat_12, SEGMENT_TITLE_COLOR);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 79, 90);

    lv_obj_t *panel = make_box(container,
                               PANEL_WIDTH,
                               PANEL_HEIGHT,
                               SEGMENT_PANEL_COLOR,
                               LV_OPA_COVER,
                               16);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_color(panel, SEGMENT_PANEL_BORDER_COLOR, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, PANEL_X, PANEL_Y);

    s_time_fg = make_label(panel, "88:88", &lv_font_seg_96, SEGMENT_TIME_COLOR);
    lv_obj_center(s_time_fg);

    s_date_fg = make_label(container, "--- -- ---", &lv_font_montserrat_24, SEGMENT_DATE_COLOR);
    lv_obj_align(s_date_fg, LV_ALIGN_TOP_LEFT, 64, 292);

    lv_obj_t *seconds_badge = make_box(container,
                                       72,
                                       48,
                                       SEGMENT_ACCENT_COLOR,
                                       LV_OPA_COVER,
                                       10);
    lv_obj_align(seconds_badge, LV_ALIGN_TOP_LEFT, 330, 284);

    s_seconds_fg = make_label(seconds_badge, "88", &lv_font_seg_36, lv_color_hex(0x000000));
    lv_obj_center(s_seconds_fg);

    lv_obj_t *track = make_box(container,
                               PROGRESS_WIDTH,
                               PROGRESS_HEIGHT,
                               SEGMENT_TRACK_COLOR,
                               LV_OPA_COVER,
                               PROGRESS_HEIGHT / 2);
    lv_obj_align(track, LV_ALIGN_TOP_LEFT, 64, 372);

    s_progress_fill = make_box(container,
                               0,
                               PROGRESS_HEIGHT,
                               SEGMENT_ACCENT_COLOR,
                               LV_OPA_COVER,
                               PROGRESS_HEIGHT / 2);
    lv_obj_align(s_progress_fill, LV_ALIGN_TOP_LEFT, 64, 372);

    return container;
}

static bool segment_update(const struct tm *timeinfo)
{
    char time_text[8] = {};
    char sec_text[4] = {};
    char date_text[32] = {};

    snprintf(time_text, sizeof(time_text), "%02d:%02d",
             timeinfo->tm_hour, timeinfo->tm_min);
    snprintf(sec_text, sizeof(sec_text), "%02d", timeinfo->tm_sec);
    snprintf(date_text, sizeof(date_text), "%s %02d %s",
             kWeekdayAbbr[timeinfo->tm_wday % 7],
             timeinfo->tm_mday,
             kMonthAbbr[timeinfo->tm_mon % 12]);

    lv_label_set_text(s_time_fg, time_text);
    lv_label_set_text(s_seconds_fg, sec_text);
    lv_label_set_text(s_date_fg, date_text);
    lv_obj_set_width(s_progress_fill, (PROGRESS_WIDTH * timeinfo->tm_sec) / 60);
    return true;
}

const clock_theme_t g_clock_theme_segment = {
    .name = "segment",
    .build = segment_build,
    .update = segment_update,
};
