#include "clock_theme.h"
#include "dot_matrix_glyphs.h"

#include <cstdio>

namespace {

constexpr int32_t kThemeSize = CLOCK_THEME_SIZE;
constexpr int32_t kDotSize = clock_theme_px(4);
constexpr int32_t kPitch = clock_theme_px(6);
constexpr int32_t kSmallDotSize = clock_theme_px(4);
constexpr int32_t kSmallPitch = clock_theme_px(6);
constexpr int32_t kGlyphWidth = dot_matrix_glyphs::kWidth;
constexpr int32_t kPanelWidth = 64 * kPitch + kDotSize;
constexpr int32_t kPanelHeight = (dot_matrix_glyphs::kHeight - 1) * kPitch + kDotSize;
constexpr int32_t kPanelLeft = (kThemeSize - kPanelWidth) / 2;
constexpr int32_t kPanelTop = clock_theme_px(158);
// Glyph-grid indices; render_digits multiplies them by kPitch.
constexpr int32_t kDigitColumns[4] = {0, 16, 34, 50};
constexpr int32_t kColonColumn = 32;
constexpr int32_t kColonRows[2] = {8, 16};
constexpr int32_t kSecondsWidth = (6 + 4) * kSmallPitch + kSmallDotSize;
constexpr int32_t kSecondsHeight = (7 - 1) * kSmallPitch + kSmallDotSize;
constexpr int32_t kSecondsTop = clock_theme_px(330);

constexpr uint32_t kColorBackground = 0x000000;
constexpr uint32_t kColorCyan = 0x7CE9F7;
constexpr uint32_t kColorBlue = 0x91B5FF;
constexpr uint32_t kColorPurple = 0xC5A1FF;
constexpr uint32_t kColorColonDim = 0x4F5F82;
constexpr uint32_t kColorDate = 0xA6ABB5;
constexpr uint32_t kColorSeconds = 0xB4A0D9;

const char *const kWeekdayAbbr[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT",
};
const char *const kMonthAbbr[12] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
};

// Small seconds use separate 5x7 glyphs, 4px lamps on a 6px pitch.
constexpr uint8_t kSmallDigits[10][7] = {
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E},
    {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E},
};

struct ThemeState {
    lv_obj_t *root = nullptr;
    lv_obj_t *time_canvas = nullptr;
    lv_obj_t *seconds_canvas = nullptr;
    lv_obj_t *colon[2] = {};
    lv_obj_t *date_label = nullptr;
    lv_obj_t *fallback_time = nullptr;
    lv_obj_t *fallback_seconds = nullptr;
    int hour = -1;
    int minute = -1;
    int second = -1;
    int mday = -1;
    int month = -1;
    int wday = -1;
};
ThemeState s;

void draw_lamp(lv_layer_t *layer, int32_t x, int32_t y, int32_t size, uint32_t color)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(color);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = LV_RADIUS_CIRCLE;
    const lv_area_t area = {x, y, x + size - 1, y + size - 1};
    lv_draw_rect(layer, &dsc, &area);
}

uint32_t gradient_color(int32_t x)
{
    // The gradient belongs to the canvas, so changing digits cannot move it.
    constexpr int32_t half = (kPanelWidth - kDotSize) / 2;
    const uint32_t from = x <= half ? kColorCyan : kColorBlue;
    const uint32_t to = x <= half ? kColorBlue : kColorPurple;
    const int32_t offset = x <= half ? x : x - half;
    uint32_t color = 0;
    for (int32_t shift = 16; shift >= 0; shift -= 8) {
        const int32_t a = (from >> shift) & 0xFF;
        const int32_t b = (to >> shift) & 0xFF;
        color |= static_cast<uint32_t>(a + (b - a) * offset / half) << shift;
    }
    return color;
}

template <typename Mask, size_t Height>
void render_digits(lv_obj_t *canvas, const Mask (&glyphs)[10][Height], int32_t width,
                   int32_t pitch, int32_t size,
                   const int32_t *columns, const uint8_t *digits, int32_t count,
                   uint32_t color, bool gradient = false)
{
    lv_canvas_fill_bg(canvas, lv_color_hex(kColorBackground), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    for (int32_t digit = 0; digit < count; ++digit) {
        for (size_t row = 0; row < Height; ++row) {
            const Mask bits = glyphs[digits[digit]][row];
            for (int32_t col = 0; col < width; ++col) {
                if (bits & (1U << (width - col - 1))) {
                    const int32_t x = (columns[digit] + col) * pitch;
                    draw_lamp(&layer, x, row * pitch, size,
                              gradient ? gradient_color(x) : color);
                }
            }
        }
    }
    lv_canvas_finish_layer(canvas, &layer);
}

void canvas_delete_cb(lv_event_t *event)
{
    auto *buffer = static_cast<lv_draw_buf_t *>(lv_event_get_user_data(event));
    lv_draw_buf_destroy(buffer);
}

lv_obj_t *make_canvas(lv_obj_t *parent, int32_t width, int32_t height)
{
    lv_draw_buf_t *buffer = lv_draw_buf_create(width, height, LV_COLOR_FORMAT_RGB565,
                                              LV_STRIDE_AUTO);
    if (buffer == nullptr) {
        return nullptr;
    }
    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_draw_buf(canvas, buffer);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_fill_bg(canvas, lv_color_hex(kColorBackground), LV_OPA_COVER);
    lv_obj_add_event_cb(canvas, canvas_delete_cb, LV_EVENT_DELETE, buffer);
    return canvas;
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                     uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

void root_delete_cb(lv_event_t *event)
{
    // The delete callback on each canvas owns its draw buffer, including when
    // a partial allocation falls back to labels during construction.
    if (lv_event_get_target(event) == s.root) {
        s = ThemeState{};
    }
}

} // namespace

static void dot_matrix_reset(void)
{
    s.hour = s.minute = s.second = s.mday = s.month = s.wday = -1;
}

static lv_obj_t *dot_matrix_build(lv_obj_t *parent)
{
    s = ThemeState{};
    s.root = lv_obj_create(parent);
    lv_obj_remove_style_all(s.root);
    lv_obj_clear_flag(s.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s.root, kThemeSize, kThemeSize);
    lv_obj_center(s.root);
    lv_obj_set_style_bg_color(s.root, lv_color_hex(kColorBackground), 0);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(s.root, root_delete_cb, LV_EVENT_DELETE, nullptr);

    s.date_label = make_label(s.root, "--- -- ---", &lv_font_montserrat_34, kColorDate);
    lv_obj_set_style_text_letter_space(s.date_label, clock_theme_px(1), 0);
    lv_obj_align(s.date_label, LV_ALIGN_TOP_MID, 0, clock_theme_px(111));

    s.time_canvas = make_canvas(s.root, kPanelWidth, kPanelHeight);
    s.seconds_canvas = make_canvas(s.root, kSecondsWidth, kSecondsHeight);
    if (s.time_canvas == nullptr || s.seconds_canvas == nullptr) {
        if (s.time_canvas != nullptr) lv_obj_delete(s.time_canvas);
        if (s.seconds_canvas != nullptr) lv_obj_delete(s.seconds_canvas);
        s.time_canvas = s.seconds_canvas = nullptr;
        s.fallback_time = make_label(s.root, "--:--", &lv_font_montserrat_48, kColorBlue);
        lv_obj_align(s.fallback_time, LV_ALIGN_TOP_MID, 0, clock_theme_px(204));
        s.fallback_seconds = make_label(s.root, "--", &lv_font_montserrat_48, kColorSeconds);
        lv_obj_align(s.fallback_seconds, LV_ALIGN_TOP_MID, 0, kSecondsTop);
    } else {
        lv_obj_set_pos(s.time_canvas, kPanelLeft, kPanelTop);
        lv_obj_align(s.seconds_canvas, LV_ALIGN_TOP_MID, 0, kSecondsTop);
        // Keep the colon in the same grid as the time canvas so it stays
        // aligned when the panel position or pitch changes.
        for (int32_t i = 0; i < 2; ++i) {
            s.colon[i] = lv_obj_create(s.time_canvas);
            lv_obj_remove_style_all(s.colon[i]);
            lv_obj_set_size(s.colon[i], kDotSize, kDotSize);
            lv_obj_set_pos(s.colon[i], kColonColumn * kPitch, kColonRows[i] * kPitch);
            lv_obj_set_style_radius(s.colon[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(s.colon[i], lv_color_hex(kColorColonDim), 0);
            lv_obj_set_style_bg_opa(s.colon[i], LV_OPA_COVER, 0);
            lv_obj_clear_flag(s.colon[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(s.colon[i], LV_OBJ_FLAG_CLICKABLE);
        }
    }
    return s.root;
}

static bool dot_matrix_update(const struct tm *timeinfo)
{
    if (timeinfo == nullptr || s.root == nullptr) {
        return false;
    }
    const bool minute_changed = s.hour != timeinfo->tm_hour || s.minute != timeinfo->tm_min;
    const bool date_changed = s.mday != timeinfo->tm_mday || s.month != timeinfo->tm_mon ||
                              s.wday != timeinfo->tm_wday;
    const bool second_changed = s.second != timeinfo->tm_sec;
    if (minute_changed) {
        if (s.time_canvas != nullptr) {
            const uint8_t digits[] = {
                static_cast<uint8_t>(timeinfo->tm_hour / 10),
                static_cast<uint8_t>(timeinfo->tm_hour % 10),
                static_cast<uint8_t>(timeinfo->tm_min / 10),
                static_cast<uint8_t>(timeinfo->tm_min % 10),
            };
            render_digits(s.time_canvas, dot_matrix_glyphs::kDigits, kGlyphWidth,
                          kPitch, kDotSize, kDigitColumns, digits, 4, kColorBlue, true);
        } else {
            char text[8];
            snprintf(text, sizeof(text), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
            clock_theme_set_label_text(s.fallback_time, text);
        }
    }
    if (date_changed) {
        char text[24];
        snprintf(text, sizeof(text), "%s %02d %s", kWeekdayAbbr[timeinfo->tm_wday % 7],
                 timeinfo->tm_mday, kMonthAbbr[timeinfo->tm_mon % 12]);
        clock_theme_set_label_text(s.date_label, text);
    }
    if (second_changed) {
        if (s.seconds_canvas != nullptr) {
            constexpr int32_t columns[] = {0, 6};
            const uint8_t digits[] = {static_cast<uint8_t>(timeinfo->tm_sec / 10),
                                      static_cast<uint8_t>(timeinfo->tm_sec % 10)};
            render_digits(s.seconds_canvas, kSmallDigits, 5, kSmallPitch, kSmallDotSize,
                          columns, digits, 2, kColorSeconds);
            for (auto *lamp : s.colon) {
                lv_obj_set_style_bg_color(lamp,
                    lv_color_hex(timeinfo->tm_sec % 2 == 0 ? kColorBlue : kColorColonDim), 0);
            }
        } else {
            char text[4];
            snprintf(text, sizeof(text), "%02d", timeinfo->tm_sec);
            clock_theme_set_label_text(s.fallback_seconds, text);
        }
    }
    s.hour = timeinfo->tm_hour;
    s.minute = timeinfo->tm_min;
    s.second = timeinfo->tm_sec;
    s.mday = timeinfo->tm_mday;
    s.month = timeinfo->tm_mon;
    s.wday = timeinfo->tm_wday;
    return minute_changed || date_changed || second_changed;
}

const clock_theme_t g_clock_theme_dot_matrix = {
    .name = "dot_matrix",
    .build = dot_matrix_build,
    .update = dot_matrix_update,
    .reset = dot_matrix_reset,
};
