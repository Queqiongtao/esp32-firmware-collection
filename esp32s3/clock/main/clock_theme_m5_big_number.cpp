#include "clock_theme.h"

#include <string.h>

#include "lvgl.h"
#include "m5_watch_assets.h"

namespace {

constexpr int32_t THEME_SIZE = 466;

static const uint32_t kBigNumberPalettes[4][4] = {
    {0xCBDB8C, 0xD3EA71, 0xE9F5B8, 0x94A350},
    {0xD1F3BF, 0xCDFCB9, 0xF8FEE9, 0x84D86D},
    {0xDB9D7D, 0xE8D780, 0xF4EBC0, 0xF2B050},
    {0xB49EDB, 0x9BC1FF, 0xC4C9FA, 0xAF94E7},
};

static lv_obj_t *s_m5_big_digits[4] = {};
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
}

} // namespace

static lv_obj_t *m5_big_number_build(lv_obj_t *parent)
{
    lv_obj_t *container = make_root(parent, lv_color_hex(0x000000));
    memset(s_m5_big_digits, 0, sizeof(s_m5_big_digits));
    s_m5_big_last_hour = -1;
    s_m5_big_last_minute = -1;

    s_m5_big_digits[0] = make_big_digit(container, -42, -51, 0xCBDB8C);
    s_m5_big_digits[1] = make_big_digit(container, 42, -51, 0xD3EA71);
    s_m5_big_digits[2] = make_big_digit(container, -42, 51, 0xE9F5B8);
    s_m5_big_digits[3] = make_big_digit(container, 42, 51, 0x94A350);
    apply_big_number_palette(0);

    return container;
}

static bool m5_big_number_update(const struct tm *timeinfo)
{
    if (timeinfo->tm_hour == s_m5_big_last_hour &&
        timeinfo->tm_min == s_m5_big_last_minute) {
        return false;
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
