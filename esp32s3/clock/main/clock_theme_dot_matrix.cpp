#include "clock_theme.h"

#include "lvgl.h"

namespace {

constexpr int32_t kThemeSize = 466;
constexpr int32_t kCellSize = 6;
constexpr int32_t kCellGap = 1;
constexpr int32_t kPitch = kCellSize + kCellGap;
constexpr int32_t kCols = 59;
constexpr int32_t kRows = 20;
constexpr int32_t kPanelWidth = kCols * kPitch - kCellGap;
constexpr int32_t kPanelHeight = kRows * kPitch - kCellGap;
constexpr int32_t kPanelLeft = (kThemeSize - kPanelWidth) / 2;
constexpr int32_t kPanelTop = (kThemeSize - kPanelHeight) / 2;

constexpr int32_t kGlyphWidth = 12;
constexpr int32_t kGlyphHeight = 20;

constexpr uint32_t kColorBackground = 0x000000;
constexpr uint32_t kPalette[] = {
    0xFF6B8A, // coral pink
    0xFF9EB5, // rose
    0xFFB38A, // peach
    0xFFD36E, // butter
    0xFFF1D6, // cream
    0x8BE3C0, // mint
    0x5FC9F8, // sky
    0x8FB5FF, // periwinkle
    0xB79CFF, // lavender
    0xFF8A5B, // tangerine
};
constexpr int32_t kPaletteCount = sizeof(kPalette) / sizeof(kPalette[0]);

// 12x20 glyph masks sampled from Paytone One (SIL OFL 1.1) at a
// 200px raster size, threshold 120, resized to 12x20. Bit 11 is the
// leftmost column; each set bit becomes one 6px mosaic tile.
constexpr uint16_t kDigits[10][kGlyphHeight] = {
    {0x000, 0x060, 0x1F8, 0x1F8, 0x3FC, 0x3FC, 0x79C, 0x79E, 0x70E, 0x70E,
     0x70E, 0x70E, 0x79E, 0x79C, 0x3FC, 0x3FC, 0x1F8, 0x1F8, 0x060, 0x000},
    {0x000, 0x038, 0x0F8, 0x3F8, 0x3F8, 0x3F8, 0x0F8, 0x0F8, 0x0F8, 0x0F8,
     0x0F8, 0x0F8, 0x0F8, 0x0F8, 0x0F8, 0x3FE, 0x3FE, 0x3FE, 0x3FC, 0x000},
    {0x000, 0x0E0, 0x3F8, 0x3FC, 0x3FC, 0x3FC, 0x03C, 0x03C, 0x03C, 0x03C,
     0x078, 0x078, 0x0F0, 0x1E0, 0x3C0, 0x3FC, 0x3FC, 0x3FC, 0x3FC, 0x000},
    {0x000, 0x0E0, 0x3F8, 0x3FC, 0x3FC, 0x33C, 0x01C, 0x03C, 0x1F8, 0x1F8,
     0x1F8, 0x1FC, 0x03C, 0x01E, 0x23C, 0x3FC, 0x3FC, 0x3F8, 0x1E0, 0x000},
    {0x000, 0x038, 0x078, 0x078, 0x0F8, 0x0F8, 0x1F8, 0x1B8, 0x3B8, 0x338,
     0x73C, 0x7FE, 0x7FE, 0x7FE, 0x3FE, 0x038, 0x038, 0x038, 0x038, 0x000},
    {0x000, 0x1FC, 0x3FC, 0x3FC, 0x3FC, 0x380, 0x380, 0x3B0, 0x3FC, 0x3FC,
     0x3FC, 0x1BE, 0x01E, 0x01E, 0x33C, 0x3FC, 0x3FC, 0x3F8, 0x1E0, 0x000},
    {0x000, 0x038, 0x0FC, 0x1FC, 0x1FC, 0x3E4, 0x3C0, 0x380, 0x7B8, 0x7FC,
     0x7FE, 0x7DE, 0x78E, 0x38E, 0x39E, 0x3FE, 0x1FC, 0x1F8, 0x070, 0x000},
    {0x000, 0x3FC, 0x7FE, 0x7FE, 0x3FE, 0x03C, 0x03C, 0x07C, 0x078, 0x078,
     0x0F0, 0x0F0, 0x0F0, 0x1E0, 0x1E0, 0x3E0, 0x3C0, 0x1C0, 0x000, 0x000},
    {0x000, 0x070, 0x1F8, 0x3FC, 0x39C, 0x39C, 0x39C, 0x3F8, 0x3F8, 0x1F8,
     0x1FC, 0x3FE, 0x39E, 0x78E, 0x79E, 0x7FC, 0x3FC, 0x1F8, 0x0F0, 0x000},
    {0x000, 0x0E0, 0x1F8, 0x3F8, 0x7FC, 0x79C, 0x71C, 0x71E, 0x7BE, 0x7FE,
     0x3FE, 0x3DE, 0x01C, 0x03C, 0x27C, 0x3F8, 0x3F8, 0x3F0, 0x1C0, 0x000},
};

constexpr int32_t kDigitColumns[4] = {0, 13, 34, 47};
constexpr int32_t kColonColumn = 28;

lv_obj_t *s_canvas = nullptr;
int s_last_hour = -1;
int s_last_minute = -1;

uint8_t tile_color_index(int digit, int row, int col,
                         const uint8_t *above, uint8_t left)
{
    const uint8_t start =
        (uint8_t)((row * 73 + col * 151 + digit * 211 + 17) % kPaletteCount);
    for (int32_t step = 0; step < kPaletteCount; ++step) {
        const uint8_t index = (uint8_t)((start + step) % kPaletteCount);
        if (index == left || index == above[col]) {
            continue;
        }
        return index;
    }
    return start;
}

void draw_tile(lv_layer_t *layer, int32_t x, int32_t y, uint32_t color)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(color);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 1;

    lv_area_t area = {
        .x1 = x,
        .y1 = y,
        .x2 = x + kCellSize - 1,
        .y2 = y + kCellSize - 1,
    };
    lv_draw_rect(layer, &dsc, &area);
}

void render_time(int hour, int minute)
{
    if (s_canvas == nullptr) {
        return;
    }

    lv_canvas_fill_bg(s_canvas, lv_color_hex(kColorBackground), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);

    const uint8_t digits[4] = {
        (uint8_t)(hour / 10),
        (uint8_t)(hour % 10),
        (uint8_t)(minute / 10),
        (uint8_t)(minute % 10),
    };

    for (int32_t digit = 0; digit < 4; ++digit) {
        uint8_t above[kGlyphWidth];
        for (int32_t i = 0; i < kGlyphWidth; ++i) {
            above[i] = 0xFF;
        }

        for (int32_t row = 0; row < kGlyphHeight; ++row) {
            const uint16_t row_bits = kDigits[digits[digit]][row];
            uint8_t left = 0xFF;
            for (int32_t col = 0; col < kGlyphWidth; ++col) {
                if ((row_bits & (1U << (kGlyphWidth - 1 - col))) == 0) {
                    above[col] = 0xFF;
                    left = 0xFF;
                    continue;
                }

                const uint8_t color_index =
                    tile_color_index(digit, row, col, above, left);
                above[col] = color_index;
                left = color_index;

                draw_tile(&layer,
                          (kDigitColumns[digit] + col) * kPitch,
                          row * kPitch,
                          kPalette[color_index]);
            }
        }
    }

    for (int32_t row = 0; row < kRows; ++row) {
        if (!((row >= 4 && row <= 6) || (row >= 13 && row <= 15))) {
            continue;
        }
        for (int32_t col = 0; col < 3; ++col) {
            draw_tile(&layer,
                      (kColonColumn + col) * kPitch,
                      row * kPitch,
                      kPalette[4]);
        }
    }

    lv_canvas_finish_layer(s_canvas, &layer);
}

void canvas_delete_cb(lv_event_t *event)
{
    lv_draw_buf_t *draw_buf =
        (lv_draw_buf_t *)lv_event_get_user_data(event);
    if (draw_buf != nullptr) {
        lv_draw_buf_destroy(draw_buf);
    }
    s_canvas = nullptr;
}

} // namespace

static lv_obj_t *dot_matrix_build(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(root, kThemeSize, kThemeSize);
    lv_obj_center(root);
    lv_obj_set_style_bg_color(root, lv_color_hex(kColorBackground), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    lv_obj_t *canvas = lv_canvas_create(root);
    lv_draw_buf_t *draw_buf = lv_draw_buf_create(kPanelWidth,
                                                 kPanelHeight,
                                                 LV_COLOR_FORMAT_RGB565,
                                                 LV_STRIDE_AUTO);
    if (draw_buf == nullptr) {
        lv_obj_delete(canvas);
        s_canvas = nullptr;
        return root;
    }

    lv_canvas_set_draw_buf(canvas, draw_buf);
    lv_canvas_fill_bg(canvas, lv_color_hex(kColorBackground), LV_OPA_COVER);
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(canvas, kPanelLeft, kPanelTop);
    lv_obj_add_event_cb(canvas, canvas_delete_cb, LV_EVENT_DELETE, draw_buf);

    s_canvas = canvas;
    s_last_hour = -1;
    s_last_minute = -1;
    return root;
}

static bool dot_matrix_update(const struct tm *timeinfo)
{
    if (timeinfo == nullptr) {
        return false;
    }

    const bool changed = s_last_hour != timeinfo->tm_hour ||
                         s_last_minute != timeinfo->tm_min;
    s_last_hour = timeinfo->tm_hour;
    s_last_minute = timeinfo->tm_min;

    if (changed) {
        render_time(timeinfo->tm_hour, timeinfo->tm_min);
    }
    return changed;
}

static void dot_matrix_reset(void)
{
    s_last_hour = -1;
    s_last_minute = -1;
}

const clock_theme_t g_clock_theme_dot_matrix = {
    .name = "dot_matrix",
    .build = dot_matrix_build,
    .update = dot_matrix_update,
    .reset = dot_matrix_reset,
};
