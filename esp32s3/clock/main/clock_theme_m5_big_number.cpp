#include "clock_theme.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "m5_watch_assets.h"

namespace {

constexpr int32_t THEME_SIZE = 466;

static const char *const kWeekdayAbbr[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

static const char *const kMonthAbbr[12] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

static const uint32_t kBigNumberPalettes[4][4] = {
    {0xDDF7FF, 0x79E0FF, 0xFFD1B8, 0xFF7A59},
    {0xD9FFF2, 0x63E6BE, 0xE6D9FF, 0x9B7BFF},
    {0xFFF0C2, 0xFFC857, 0xB8E6FF, 0x5CA9E6},
    {0xFFFFFF, 0xD9D9D9, 0xFFB3C7, 0xFF5C8A},
};

static const uint32_t kBigNumberDateColors[4] = {
    0x8FA6AD, 0x9AA6B2, 0xB8A99A, 0xB0A6B8};

static const uint32_t kBigNumberRuleColors[4] = {
    0xFF7A59, 0x9B7BFF, 0x5CA9E6, 0xFF5C8A};

static lv_obj_t *s_m5_big_digits[4] = {};
static lv_obj_t *s_m5_big_date = NULL;
static lv_obj_t *s_m5_big_rule = NULL;
static int s_m5_big_last_hour = -1;
static int s_m5_big_last_minute = -1;

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
    if (s_m5_big_date != NULL) {
        lv_obj_set_style_text_color(s_m5_big_date, lv_color_hex(kBigNumberDateColors[palette_index]), 0);
    }
    if (s_m5_big_rule != NULL) {
        lv_obj_set_style_bg_color(s_m5_big_rule, lv_color_hex(kBigNumberRuleColors[palette_index]), 0);
    }
}

} // namespace

static lv_obj_t *m5_big_number_build(lv_obj_t *parent)
{
    lv_obj_t *container = make_root(parent, lv_color_hex(0x000000));
    memset(s_m5_big_digits, 0, sizeof(s_m5_big_digits));
    s_m5_big_date = NULL;
    s_m5_big_rule = NULL;
    s_m5_big_last_hour = -1;
    s_m5_big_last_minute = -1;

    s_m5_big_digits[0] = make_big_digit(container, -42, -71, 0xCBDB8C);
    s_m5_big_digits[1] = make_big_digit(container, 42, -71, 0xD3EA71);
    s_m5_big_digits[2] = make_big_digit(container, -42, 31, 0xE9F5B8);
    s_m5_big_digits[3] = make_big_digit(container, 42, 31, 0x94A350);

    s_m5_big_rule = make_panel(container, 36, 2, 1, lv_color_hex(0xFF7A59), LV_OPA_40);
    lv_obj_align(s_m5_big_rule, LV_ALIGN_CENTER, 0, 106);

    s_m5_big_date = make_label(container, &lv_font_montserrat_24, lv_color_hex(0x8FA6AD));
    lv_obj_set_style_text_letter_space(s_m5_big_date, 1, 0);
    lv_obj_set_style_text_opa(s_m5_big_date, LV_OPA_80, 0);
    lv_label_set_text(s_m5_big_date, "--- -- ---");
    lv_obj_align(s_m5_big_date, LV_ALIGN_CENTER, 0, 130);

    apply_big_number_palette(0);

    return container;
}

static bool m5_big_number_update(const struct tm *timeinfo)
{
    char date_text[16] = {};
    snprintf(date_text, sizeof(date_text), "%s %02d %s",
             kWeekdayAbbr[timeinfo->tm_wday % 7],
             timeinfo->tm_mday,
             kMonthAbbr[timeinfo->tm_mon % 12]);

    bool changed = false;
    if (s_m5_big_date != NULL &&
        strcmp(lv_label_get_text(s_m5_big_date), date_text) != 0) {
        lv_label_set_text(s_m5_big_date, date_text);
        changed = true;
    }

    if (timeinfo->tm_hour == s_m5_big_last_hour &&
        timeinfo->tm_min == s_m5_big_last_minute) {
        return changed;
    }

    s_m5_big_last_hour = timeinfo->tm_hour;
    s_m5_big_last_minute = timeinfo->tm_min;
    const int digits[4] = {
        timeinfo->tm_hour / 10,
        timeinfo->tm_hour % 10,
        timeinfo->tm_min / 10,
        timeinfo->tm_min % 10,
    };

    for (int i = 0; i < 4; ++i) {
        lv_image_set_src(s_m5_big_digits[i], get_big_number_image(digits[i]));
    }
    return true;
}

const clock_theme_t g_clock_theme_m5_big_number = {
    .name = "m5_big_number",
    .build = m5_big_number_build,
    .update = m5_big_number_update,
};
