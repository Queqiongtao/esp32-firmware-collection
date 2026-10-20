#include "clock_theme.h"

#include <stdio.h>

#include "lvgl.h"

extern "C" const lv_font_t lv_font_seg_96;
extern "C" const lv_font_t lv_font_seg_36;

namespace {

constexpr int32_t THEME_SIZE = 466;

#define HUD_BG_COLOR lv_color_hex(0x000000)
#define HUD_RING_COLOR lv_color_hex(0x1BC7E8)
#define HUD_PRIMARY_COLOR lv_color_hex(0x48F3FF)
#define HUD_SECONDARY_COLOR lv_color_hex(0xB7F7FF)
#define HUD_DIM_COLOR lv_color_hex(0x0B6F86)
#define HUD_ACCENT_COLOR lv_color_hex(0xFFFFFF)
#define HUD_PANEL_COLOR lv_color_hex(0x06131A)
#define HUD_GHOST_OPA LV_OPA_10

static lv_obj_t *s_time_fg = NULL;
static lv_obj_t *s_seconds_fg = NULL;
static lv_obj_t *s_date_fg = NULL;

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

static lv_obj_t *make_bar(lv_obj_t *parent,
                          int32_t x,
                          int32_t y,
                          int32_t width,
                          int32_t height,
                          lv_color_t color,
                          lv_opa_t opa)
{
    lv_obj_t *bar = make_box(parent, width, height, color, opa, 0);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, x, y);
    return bar;
}

static lv_obj_t *make_label(lv_obj_t *parent,
                            const char *text,
                            const lv_font_t *font,
                            lv_color_t color,
                            lv_opa_t opa)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *make_seg_field(lv_obj_t *parent,
                                const lv_font_t *font,
                                const char *ghost_text,
                                int32_t width,
                                int32_t height,
                                lv_color_t color,
                                lv_opa_t ghost_opa)
{
    lv_obj_t *stack = lv_obj_create(parent);
    lv_obj_remove_style_all(stack);
    lv_obj_clear_flag(stack, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(stack, width, height);

    lv_obj_t *ghost = make_label(stack, ghost_text, font, HUD_DIM_COLOR, ghost_opa);
    lv_obj_center(ghost);

    lv_obj_t *fg = make_label(stack, ghost_text, font, color, LV_OPA_COVER);
    lv_obj_center(fg);

    return fg;
}

static void build_segment_panel_chrome(lv_obj_t *container)
{
    make_bar(container, 62, 66, 96, 2, HUD_PRIMARY_COLOR, LV_OPA_60);
    make_bar(container, 308, 66, 96, 2, HUD_PRIMARY_COLOR, LV_OPA_60);
    make_bar(container, 62, 392, 342, 1, HUD_DIM_COLOR, LV_OPA_30);

    make_bar(container, 116, 116, 34, 2, HUD_DIM_COLOR, LV_OPA_30);
    make_bar(container, 316, 116, 34, 2, HUD_DIM_COLOR, LV_OPA_30);
    make_bar(container, 116, 244, 34, 2, HUD_DIM_COLOR, LV_OPA_20);
    make_bar(container, 316, 244, 34, 2, HUD_DIM_COLOR, LV_OPA_20);
}

} // namespace

static lv_obj_t *segment_build(lv_obj_t *parent)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(container, THEME_SIZE, THEME_SIZE);
    lv_obj_set_style_bg_color(container, HUD_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_center(container);

    build_segment_panel_chrome(container);

    lv_obj_t *title = make_label(container, "SEGMENT CORE", &lv_font_montserrat_16, HUD_SECONDARY_COLOR, LV_OPA_50);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 58);

    s_time_fg = make_seg_field(container,
                               &lv_font_seg_96,
                               "88:88",
                               342,
                               112,
                               HUD_PRIMARY_COLOR,
                               HUD_GHOST_OPA);
    lv_obj_align(lv_obj_get_parent(s_time_fg), LV_ALIGN_TOP_MID, 0, 132);

    lv_obj_t *sec_caption = make_label(container, "SEC", &lv_font_montserrat_12, HUD_SECONDARY_COLOR, LV_OPA_50);
    lv_obj_set_style_text_letter_space(sec_caption, 1, 0);
    lv_obj_align(sec_caption, LV_ALIGN_TOP_MID, -94, 286);

    s_seconds_fg = make_seg_field(container,
                                  &lv_font_seg_36,
                                  "88",
                                  64,
                                  42,
                                  HUD_ACCENT_COLOR,
                                  HUD_GHOST_OPA);
    lv_obj_align(lv_obj_get_parent(s_seconds_fg), LV_ALIGN_TOP_MID, -94, 307);

    lv_obj_t *date_caption = make_label(container, "DATE", &lv_font_montserrat_12, HUD_SECONDARY_COLOR, LV_OPA_50);
    lv_obj_set_style_text_letter_space(date_caption, 1, 0);
    lv_obj_align(date_caption, LV_ALIGN_TOP_MID, 78, 286);

    s_date_fg = make_seg_field(container,
                               &lv_font_seg_36,
                               "88-88",
                               150,
                               42,
                               HUD_SECONDARY_COLOR,
                               HUD_GHOST_OPA);
    lv_obj_align(lv_obj_get_parent(s_date_fg), LV_ALIGN_TOP_MID, 78, 307);

    lv_obj_t *footer = make_label(container, "ESP CLOCK / S3 HUD", &lv_font_montserrat_12, HUD_DIM_COLOR, LV_OPA_50);
    lv_obj_set_style_text_letter_space(footer, 1, 0);
    lv_obj_align(footer, LV_ALIGN_TOP_MID, 0, 376);

    return container;
}

static bool segment_update(const struct tm *timeinfo)
{
    char time_text[8] = {};
    char sec_text[4] = {};
    char date_text[8] = {};

    snprintf(time_text, sizeof(time_text), "%02d:%02d",
             timeinfo->tm_hour, timeinfo->tm_min);
    snprintf(sec_text, sizeof(sec_text), "%02d", timeinfo->tm_sec);
    snprintf(date_text, sizeof(date_text), "%02d-%02d",
             timeinfo->tm_mon + 1, timeinfo->tm_mday);

    lv_label_set_text(s_time_fg, time_text);
    lv_label_set_text(s_seconds_fg, sec_text);
    lv_label_set_text(s_date_fg, date_text);
    return true;
}

const clock_theme_t g_clock_theme_segment = {
    .name = "segment",
    .build = segment_build,
    .update = segment_update,
};
