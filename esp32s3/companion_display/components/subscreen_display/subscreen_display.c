#include "subscreen_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_amoled_1_8.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "jpeg_decoder.h"

static const char *TAG = "subscreen_display";

#define PLACEHOLDER_CHUNK_LINES 32
#define DECODE_BUFFER_COUNT 2
#define JPEG_WORKING_BUFFER_SIZE 65472

/*
 * SH8601 QSPI color writes consume RGB565 as high byte then low byte. ESP32-S3
 * stores uint16_t values little-endian, so local UI constants are converted at
 * compile time to the byte layout expected by the panel. JPEG decode produces
 * the same layout through swap_color_bytes.
 */
#define SH8601_RGB565(logical_rgb565) __builtin_bswap16((uint16_t)(logical_rgb565))

#define COLOR_BG SH8601_RGB565(0x18E3)
#define COLOR_SURFACE SH8601_RGB565(0x3906)
#define COLOR_TRACK SH8601_RGB565(0x42C8)
#define COLOR_TEXT SH8601_RGB565(0xEF7D)
#define COLOR_MUTED SH8601_RGB565(0x9552)
#define COLOR_ACCENT SH8601_RGB565(0x36FB)
#define COLOR_INFO SH8601_RGB565(0x36FB)
#define COLOR_SUCCESS SH8601_RGB565(0x36C0)
#define COLOR_WARNING SH8601_RGB565(0xFB60)
#define COLOR_DANGER SH8601_RGB565(0xEA00)
#define COLOR_MAGENTA SH8601_RGB565(0xF817)
#define COLOR_CARD COLOR_SURFACE

#define LAYOUT_FOOTER_HEIGHT 32
#define LAYOUT_MARGIN 20
#define LAYOUT_CARD_RADIUS 8
#define LAYOUT_CARD_BORDER 2
#define LAYOUT_TITLE_Y 16
#define LAYOUT_TITLE_UNDERLINE_Y 40
#define LAYOUT_TITLE_UNDERLINE_H 2
#define LAYOUT_TITLE_UNDERLINE_W 96

struct subscreen_display {
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t panel_io;
    int physical_width;
    int physical_height;
    uint16_t *decoded_frames[DECODE_BUFFER_COUNT];
    void *jpeg_working_buffer;
    uint8_t next_decode_buffer;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t color_done;
    bool color_transfer_pending;
    int64_t color_submit_us;
    subscreen_transport_status_t transport_status;
    subscreen_network_status_t network_status;
    char network_ipv4[16];
    char pairing_token[7];
    bool has_frame;
};

static struct subscreen_display s_display;

static bool color_transfer_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                   esp_lcd_panel_io_event_data_t *edata,
                                   void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    subscreen_display_t *display = user_ctx;
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(display->color_done, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static esp_err_t wait_color_transfer_locked(subscreen_display_t *display,
                                            uint32_t *wait_us,
                                            uint32_t *dma_us)
{
    if (!display->color_transfer_pending) {
        return ESP_OK;
    }

    const int64_t wait_started_us = esp_timer_get_time();
    ESP_RETURN_ON_FALSE(xSemaphoreTake(display->color_done, portMAX_DELAY) == pdTRUE,
                        ESP_FAIL, TAG, "wait color transfer failed");
    const int64_t completed_us = esp_timer_get_time();
    if (wait_us != NULL) {
        *wait_us = (uint32_t)(completed_us - wait_started_us);
    }
    if (dma_us != NULL) {
        *dma_us = (uint32_t)(completed_us - display->color_submit_us);
    }
    display->color_transfer_pending = false;
    return ESP_OK;
}

static esp_err_t submit_bitmap_locked(subscreen_display_t *display,
                                      int y_start,
                                      int y_end,
                                      const void *pixels)
{
    ESP_RETURN_ON_FALSE(!display->color_transfer_pending, ESP_ERR_INVALID_STATE, TAG,
                        "color transfer still pending");
    while (xSemaphoreTake(display->color_done, 0) == pdTRUE) {
    }
    display->color_submit_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(display->panel, 0, y_start,
                                                   display->physical_width, y_end, pixels),
                        TAG, "draw bitmap failed");
    display->color_transfer_pending = true;
    return ESP_OK;
}

static const uint8_t s_font_5x7[][5] = {
    {0x1e, 0x05, 0x05, 0x05, 0x1e}, {0x1f, 0x15, 0x15, 0x15, 0x0a},
    {0x0e, 0x11, 0x11, 0x11, 0x0a}, {0x1f, 0x11, 0x11, 0x0a, 0x04},
    {0x1f, 0x15, 0x15, 0x15, 0x11}, {0x1f, 0x05, 0x05, 0x05, 0x01},
    {0x0e, 0x11, 0x15, 0x15, 0x1d}, {0x1f, 0x04, 0x04, 0x04, 0x1f},
    {0x11, 0x11, 0x1f, 0x11, 0x11}, {0x08, 0x10, 0x11, 0x11, 0x0f},
    {0x1f, 0x04, 0x0a, 0x11, 0x00}, {0x1f, 0x10, 0x10, 0x10, 0x10},
    {0x1f, 0x02, 0x04, 0x02, 0x1f}, {0x1f, 0x02, 0x04, 0x08, 0x1f},
    {0x0e, 0x11, 0x11, 0x11, 0x0e}, {0x1f, 0x05, 0x05, 0x05, 0x02},
    {0x0e, 0x11, 0x19, 0x11, 0x1e}, {0x1f, 0x05, 0x0d, 0x15, 0x02},
    {0x12, 0x15, 0x15, 0x15, 0x09}, {0x01, 0x01, 0x1f, 0x01, 0x01},
    {0x0f, 0x10, 0x10, 0x10, 0x0f}, {0x07, 0x08, 0x10, 0x08, 0x07},
    {0x1f, 0x08, 0x04, 0x08, 0x1f}, {0x1b, 0x04, 0x04, 0x04, 0x1b},
    {0x03, 0x04, 0x18, 0x04, 0x03}, {0x19, 0x15, 0x15, 0x13, 0x00},
    {0x3e, 0x41, 0x41, 0x41, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00},
    {0x62, 0x51, 0x49, 0x49, 0x47}, {0x22, 0x41, 0x49, 0x49, 0x36},
    {0x18, 0x14, 0x12, 0x7f, 0x00}, {0x27, 0x45, 0x45, 0x7d, 0x39},
    {0x3e, 0x49, 0x49, 0x79, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x79, 0x3e},
    {0x00, 0x60, 0x60, 0x00, 0x00}, {0x00, 0x00, 0x0a, 0x00, 0x00},
    {0x00, 0x00, 0x0a, 0x00, 0x00},
};

static const uint8_t *font_glyph(char c)
{
    if (c >= 'A' && c <= 'Z') return s_font_5x7[c - 'A'];
    if (c >= '0' && c <= '9') return s_font_5x7[26 + c - '0'];
    if (c == '.') return s_font_5x7[36];
    if (c == ':') return s_font_5x7[37];
    return NULL;
}

static void draw_text(uint16_t *pixels, int width, int height, int x, int y,
                      int scale, const char *text, uint16_t color)
{
    for (const char *p = text; *p != '\0'; ++p, x += 6 * scale) {
        const uint8_t *glyph = font_glyph(*p);
        if (glyph == NULL) continue;
        for (int col = 0; col < 5; ++col) {
            for (int row = 0; row < 7; ++row) {
                if ((glyph[col] & (1U << row)) == 0) continue;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        const int px = x + col * scale + dx;
                        const int py = y + row * scale + dy;
                        if (px >= 0 && px < width && py >= 0 && py < height) pixels[py * width + px] = color;
                    }
                }
            }
        }
    }
}

static int text_width(const char *text, int scale)
{
    return (int)strlen(text) * 6 * scale;
}

static void draw_centered_text(uint16_t *pixels, int width, int height, int y,
                               int scale, const char *text, uint16_t color)
{
    draw_text(pixels, width, height, (width - text_width(text, scale)) / 2, y, scale, text, color);
}

static void fill_rect(uint16_t *pixels, int width, int height, int x, int y,
                      int rect_width, int rect_height, uint16_t color)
{
    const int x_end = x + rect_width < width ? x + rect_width : width;
    const int y_end = y + rect_height < height ? y + rect_height : height;
    for (int py = y > 0 ? y : 0; py < y_end; ++py) {
        uint16_t *row = pixels + py * width;
        for (int px = x > 0 ? x : 0; px < x_end; ++px) {
            row[px] = color;
        }
    }
}

static void set_pixel(uint16_t *pixels, int width, int height, int x, int y, uint16_t color)
{
    if (x < 0 || x >= width || y < 0 || y >= height) return;
    pixels[y * width + x] = color;
}

static void fill_rounded_rect(uint16_t *pixels, int width, int height, int x, int y,
                              int rect_width, int rect_height, int radius, uint16_t color)
{
    if (radius < 0) radius = 0;
    if (radius > rect_width / 2) radius = rect_width / 2;
    if (radius > rect_height / 2) radius = rect_height / 2;
    fill_rect(pixels, width, height, x, y, rect_width, rect_height, color);
    if (radius == 0) return;
    const int center = 2 * radius - 1;
    const int r2 = center * center;
    for (int cy = 0; cy < radius; ++cy) {
        const int dy = 2 * cy - center;
        for (int cx = 0; cx < radius; ++cx) {
            const int dx = 2 * cx - center;
            if (dx * dx + dy * dy > r2) {
                set_pixel(pixels, width, height, x + cx, y + cy, COLOR_BG);
                set_pixel(pixels, width, height, x + rect_width - 1 - cx, y + cy, COLOR_BG);
                set_pixel(pixels, width, height, x + cx, y + rect_height - 1 - cy, COLOR_BG);
                set_pixel(pixels, width, height, x + rect_width - 1 - cx, y + rect_height - 1 - cy, COLOR_BG);
            }
        }
    }
}

static void draw_card(uint16_t *pixels, int width, int height, int y, int card_height)
{
    const int x = LAYOUT_MARGIN;
    const int card_width = width - 2 * LAYOUT_MARGIN;
    fill_rounded_rect(pixels, width, height, x, y, card_width, card_height,
                      LAYOUT_CARD_RADIUS, COLOR_TRACK);
    fill_rounded_rect(pixels, width, height, x + LAYOUT_CARD_BORDER, y + LAYOUT_CARD_BORDER,
                      card_width - 2 * LAYOUT_CARD_BORDER, card_height - 2 * LAYOUT_CARD_BORDER,
                      LAYOUT_CARD_RADIUS - LAYOUT_CARD_BORDER, COLOR_CARD);
}

typedef struct {
    const char *text;
    int scale;
    uint16_t color;
    int gap_before;
} card_line_t;

#define FONT_GLYPH_HEIGHT 7

static void draw_card_with_lines(uint16_t *pixels, int width, int height, int y,
                                 int card_height, const card_line_t *lines, int line_count)
{
    draw_card(pixels, width, height, y, card_height);
    int content_height = 0;
    for (int i = 0; i < line_count; ++i) {
        content_height += lines[i].gap_before + FONT_GLYPH_HEIGHT * lines[i].scale;
    }
    int text_y = y + (card_height - content_height) / 2;
    for (int i = 0; i < line_count; ++i) {
        text_y += lines[i].gap_before;
        draw_centered_text(pixels, width, height, text_y, lines[i].scale, lines[i].text, lines[i].color);
        text_y += FONT_GLYPH_HEIGHT * lines[i].scale;
    }
}

static void draw_section_label(uint16_t *pixels, int width, int height, int y,
                               const char *text)
{
    const int scale = 2;
    const int x = LAYOUT_MARGIN + 6;
    draw_text(pixels, width, height, x, y, scale, text, COLOR_MUTED);
    const int label_w = text_width(text, scale);
    const int bar_y = y + (FONT_GLYPH_HEIGHT * scale) / 2;
    fill_rect(pixels, width, height, x + label_w + 8, bar_y,
              width - LAYOUT_MARGIN - (x + label_w + 8), 1, COLOR_TRACK);
}

static void draw_network_status(uint16_t *pixels, int width, int height,
                                const subscreen_display_t *display)
{
    switch (display->network_status) {
    case SUBSCREEN_NETWORK_CONNECTED: {
        draw_section_label(pixels, width, height, 64, "NETWORK");
        const card_line_t network_lines[] = {
            {"WIFI CONNECTED", 3, COLOR_SUCCESS, 0},
            {display->network_ipv4, 4, COLOR_TEXT, 18},
        };
        draw_card_with_lines(pixels, width, height, 76, 120, network_lines, 2);
        if (strlen(display->pairing_token) == 6) {
            draw_section_label(pixels, width, height, 210, "PAIRING");
            const card_line_t pairing_lines[] = {
                {"PAIRING CODE", 3, COLOR_ACCENT, 0},
                {display->pairing_token, 8, COLOR_TEXT, 22},
            };
            draw_card_with_lines(pixels, width, height, 222, 150, pairing_lines, 2);
        }
        draw_centered_text(pixels, width, height, 388, 2, "ENTER IP AND CODE ON PC", COLOR_MUTED);
        break;
    }
    case SUBSCREEN_NETWORK_CONNECTING: {
        const card_line_t lines[] = {
            {"WIFI CONNECTING", 3, COLOR_WARNING, 0},
            {"WAITING FOR IP", 3, COLOR_TEXT, 22},
        };
        draw_card_with_lines(pixels, width, height, 140, 110, lines, 2);
        break;
    }
    case SUBSCREEN_NETWORK_SETUP_REQUIRED:
    default: {
        const card_line_t lines[] = {
            {"WIFI SETUP REQUIRED", 3, COLOR_WARNING, 0},
            {"CONNECT TO", 3, COLOR_TEXT, 22},
            {"COMPANION AP", 3, COLOR_TEXT, 6},
            {"OPEN 192.168.4.1", 3, COLOR_ACCENT, 22},
        };
        draw_card_with_lines(pixels, width, height, 116, 220, lines, 4);
        break;
    }
    }
}

static uint16_t transport_status_color(subscreen_transport_status_t status)
{
    switch (status) {
    case SUBSCREEN_TRANSPORT_HTTP_STATUS_CHECK:
        return COLOR_WARNING;
    case SUBSCREEN_TRANSPORT_WS_CONNECTED:
    case SUBSCREEN_TRANSPORT_WAITING_FRAME:
    case SUBSCREEN_TRANSPORT_NEGOTIATING:
    case SUBSCREEN_TRANSPORT_TCP_VIDEO:
    case SUBSCREEN_TRANSPORT_UDP_VIDEO:
        return COLOR_INFO;
    case SUBSCREEN_TRANSPORT_RECEIVING_FRAME:
        return COLOR_SUCCESS;
    case SUBSCREEN_TRANSPORT_BAD_PAIRING_CODE:
    case SUBSCREEN_TRANSPORT_DEVICE_BUSY:
    case SUBSCREEN_TRANSPORT_BAD_WS_FRAME:
    case SUBSCREEN_TRANSPORT_BAD_SIZE:
    case SUBSCREEN_TRANSPORT_BAD_HEADER:
    case SUBSCREEN_TRANSPORT_BAD_CRC:
    case SUBSCREEN_TRANSPORT_NO_BUFFER:
    case SUBSCREEN_TRANSPORT_FRAME_REJECTED:
    case SUBSCREEN_TRANSPORT_UDP_DROPPED:
    case SUBSCREEN_TRANSPORT_UDP_REJECTED:
    case SUBSCREEN_TRANSPORT_SESSION_EXPIRED:
    case SUBSCREEN_TRANSPORT_STREAM_TIMEOUT:
        return COLOR_DANGER;
    case SUBSCREEN_TRANSPORT_FRAME_FAILED:
        return COLOR_MAGENTA;
    case SUBSCREEN_TRANSPORT_READY:
    default:
        return COLOR_ACCENT;
    }
}

static const char *transport_status_text(subscreen_transport_status_t status)
{
    switch (status) {
    case SUBSCREEN_TRANSPORT_HTTP_STATUS_CHECK:
        return "HTTP STATUS CHECK";
    case SUBSCREEN_TRANSPORT_WS_CONNECTED:
        return "WS CONNECTED";
    case SUBSCREEN_TRANSPORT_NEGOTIATING:
        return "NEGOTIATING";
    case SUBSCREEN_TRANSPORT_TCP_VIDEO:
        return "TCP VIDEO";
    case SUBSCREEN_TRANSPORT_UDP_VIDEO:
        return "UDP VIDEO";
    case SUBSCREEN_TRANSPORT_WAITING_FRAME:
        return "WAITING FIRST FRAME";
    case SUBSCREEN_TRANSPORT_RECEIVING_FRAME:
        return "RECEIVING FRAME";
    case SUBSCREEN_TRANSPORT_BAD_PAIRING_CODE:
        return "BAD PAIRING CODE";
    case SUBSCREEN_TRANSPORT_DEVICE_BUSY:
        return "DEVICE BUSY";
    case SUBSCREEN_TRANSPORT_BAD_WS_FRAME:
        return "BAD WS FRAME";
    case SUBSCREEN_TRANSPORT_BAD_SIZE:
        return "BAD SIZE";
    case SUBSCREEN_TRANSPORT_BAD_HEADER:
        return "BAD HEADER";
    case SUBSCREEN_TRANSPORT_BAD_CRC:
        return "BAD CRC";
    case SUBSCREEN_TRANSPORT_NO_BUFFER:
        return "NO BUFFER";
    case SUBSCREEN_TRANSPORT_FRAME_REJECTED:
        return "FRAME REJECTED";
    case SUBSCREEN_TRANSPORT_FRAME_FAILED:
        return "FRAME DRAW FAILED";
    case SUBSCREEN_TRANSPORT_UDP_DROPPED:
        return "UDP FRAME DROPPED";
    case SUBSCREEN_TRANSPORT_UDP_REJECTED:
        return "UDP REJECTED";
    case SUBSCREEN_TRANSPORT_SESSION_EXPIRED:
        return "SESSION EXPIRED";
    case SUBSCREEN_TRANSPORT_STREAM_TIMEOUT:
        return "STREAM TIMEOUT";
    case SUBSCREEN_TRANSPORT_READY:
    default:
        return "READY";
    }
}

static esp_err_t draw_placeholder_locked(subscreen_display_t *display)
{
    ESP_RETURN_ON_ERROR(wait_color_transfer_locked(display, NULL, NULL), TAG,
                        "wait previous frame failed");
    const int width = display->physical_width;
    const int height = display->physical_height;
    const size_t pixel_count = (size_t)width * height;
    uint16_t *pixels = heap_caps_malloc(pixel_count * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(pixels != NULL, ESP_ERR_NO_MEM, TAG, "placeholder buffer alloc failed");

    fill_rect(pixels, width, height, 0, 0, width, height, COLOR_BG);
    draw_centered_text(pixels, width, height, LAYOUT_TITLE_Y, 3,
                       "COMPANION DISPLAY", COLOR_ACCENT);
    fill_rect(pixels, width, height,
              (width - LAYOUT_TITLE_UNDERLINE_W) / 2, LAYOUT_TITLE_UNDERLINE_Y,
              LAYOUT_TITLE_UNDERLINE_W, LAYOUT_TITLE_UNDERLINE_H, COLOR_ACCENT);
    draw_network_status(pixels, width, height, display);
    const int footer_top = height - LAYOUT_FOOTER_HEIGHT;
    fill_rect(pixels, width, height, 0, footer_top, width, LAYOUT_FOOTER_HEIGHT, COLOR_SURFACE);
    fill_rect(pixels, width, height, 0, footer_top, width, 2,
              transport_status_color(display->transport_status));
    draw_centered_text(pixels, width, height,
                       footer_top + 6 + (LAYOUT_FOOTER_HEIGHT - 6 - FONT_GLYPH_HEIGHT * 2) / 2, 2,
                       transport_status_text(display->transport_status), COLOR_TEXT);

    for (int y = 0; y < height; y += PLACEHOLDER_CHUNK_LINES) {
        const int y2 = (y + PLACEHOLDER_CHUNK_LINES < height) ? y + PLACEHOLDER_CHUNK_LINES : height;
        esp_err_t err = submit_bitmap_locked(display, y, y2, pixels + y * width);
        if (err == ESP_OK) {
            err = wait_color_transfer_locked(display, NULL, NULL);
        }
        if (err != ESP_OK) {
            free(pixels);
            return err;
        }
    }

    free(pixels);
    return ESP_OK;
}

esp_err_t subscreen_display_init(subscreen_display_t **ret_display)
{
    ESP_RETURN_ON_FALSE(ret_display != NULL, ESP_ERR_INVALID_ARG, TAG, "ret_display is null");
    if (s_display.panel != NULL) {
        *ret_display = &s_display;
        return ESP_OK;
    }

    const board_amoled_1_8_config_t *cfg = board_amoled_1_8_get_config();
    ESP_RETURN_ON_ERROR(board_display_init(), TAG, "board display init failed");
    ESP_RETURN_ON_ERROR(board_display_new(&s_display.panel, &s_display.panel_io), TAG, "get panel failed");

    s_display.physical_width = cfg->lcd_h_res;
    s_display.physical_height = cfg->lcd_v_res;
    const size_t pixel_count = (size_t)s_display.physical_width * s_display.physical_height;
    s_display.lock = xSemaphoreCreateMutex();
    s_display.color_done = xSemaphoreCreateBinary();
    for (size_t i = 0; i < DECODE_BUFFER_COUNT; ++i) {
        s_display.decoded_frames[i] = heap_caps_aligned_alloc(16, pixel_count * sizeof(uint16_t),
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    s_display.jpeg_working_buffer = heap_caps_aligned_alloc(16, JPEG_WORKING_BUFFER_SIZE,
                                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_display.decoded_frames[0] != NULL && s_display.decoded_frames[1] != NULL &&
                        s_display.jpeg_working_buffer != NULL && s_display.lock != NULL &&
                        s_display.color_done != NULL,
                        ESP_ERR_NO_MEM, TAG, "display buffers alloc failed");
    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = color_transfer_done_cb,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(s_display.panel_io, &callbacks, &s_display),
                        TAG, "register color callback failed");

    *ret_display = &s_display;
    ESP_LOGI(TAG, "ready: physical=%dx%d", s_display.physical_width, s_display.physical_height);
    return ESP_OK;
}

esp_err_t subscreen_display_show_placeholder(subscreen_display_t *display)
{
    return subscreen_display_return_to_placeholder(display, SUBSCREEN_TRANSPORT_READY);
}

esp_err_t subscreen_display_return_to_placeholder(subscreen_display_t *display,
                                                  subscreen_transport_status_t status)
{
    ESP_RETURN_ON_FALSE(display != NULL && display->lock != NULL, ESP_ERR_INVALID_STATE, TAG, "display not ready");
    xSemaphoreTake(display->lock, portMAX_DELAY);
    display->transport_status = status;
    esp_err_t err = draw_placeholder_locked(display);
    if (err == ESP_OK) {
        display->has_frame = false;
    }
    xSemaphoreGive(display->lock);
    return err;
}

esp_err_t subscreen_display_set_transport_status(subscreen_display_t *display,
                                                 subscreen_transport_status_t status)
{
    ESP_RETURN_ON_FALSE(display != NULL && display->lock != NULL, ESP_ERR_INVALID_STATE, TAG, "display not ready");
    xSemaphoreTake(display->lock, portMAX_DELAY);
    const bool changed = display->transport_status != status;
    display->transport_status = status;
    esp_err_t err = ESP_OK;
    if (changed && !display->has_frame) {
        err = draw_placeholder_locked(display);
    }
    xSemaphoreGive(display->lock);
    return err;
}

esp_err_t subscreen_display_set_network_status(subscreen_display_t *display,
                                               subscreen_network_status_t status,
                                               const char *ipv4_address,
                                               const char *pairing_token)
{
    ESP_RETURN_ON_FALSE(display != NULL && display->lock != NULL, ESP_ERR_INVALID_STATE, TAG, "display not ready");
    xSemaphoreTake(display->lock, portMAX_DELAY);
    const char *ip = ipv4_address != NULL ? ipv4_address : "";
    const char *token = pairing_token != NULL ? pairing_token : "";
    const bool changed = display->network_status != status ||
                         strcmp(display->network_ipv4, ip) != 0 ||
                         strcmp(display->pairing_token, token) != 0;
    display->network_status = status;
    strlcpy(display->network_ipv4, ip, sizeof(display->network_ipv4));
    strlcpy(display->pairing_token, token, sizeof(display->pairing_token));
    esp_err_t err = ESP_OK;
    if (changed && !display->has_frame) {
        err = draw_placeholder_locked(display);
    }
    xSemaphoreGive(display->lock);
    return err;
}

esp_err_t subscreen_display_draw_jpeg(subscreen_display_t *display,
                                      const uint8_t *jpeg,
                                      size_t jpeg_len,
                                      uint16_t width,
                                      uint16_t height,
                                      subscreen_display_frame_metrics_t *metrics)
{
    ESP_RETURN_ON_FALSE(display != NULL && jpeg != NULL && jpeg_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid JPEG");
    ESP_RETURN_ON_FALSE(subscreen_display_supports_frame_size(display, width, height), ESP_ERR_INVALID_SIZE, TAG,
                        "unexpected frame size %ux%u", width, height);

    xSemaphoreTake(display->lock, portMAX_DELAY);
    subscreen_display_frame_metrics_t frame_metrics = {};
    uint16_t *decoded_frame = display->decoded_frames[display->next_decode_buffer];
    const int64_t decode_started_us = esp_timer_get_time();
    esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = jpeg_len,
        .outbuf = (uint8_t *)decoded_frame,
        .outbuf_size = (size_t)display->physical_width * display->physical_height * sizeof(uint16_t),
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = {.swap_color_bytes = 1},
        .advanced = {
            .working_buffer = display->jpeg_working_buffer,
            .working_buffer_size = JPEG_WORKING_BUFFER_SIZE,
        },
    };
    esp_jpeg_image_output_t output = {};
    esp_err_t err = esp_jpeg_decode(&jpeg_cfg, &output);
    if (err != ESP_OK || output.width != width || output.height != height) {
        ESP_LOGW(TAG, "JPEG decode failed: err=%s size=%ux%u", esp_err_to_name(err), output.width, output.height);
        xSemaphoreGive(display->lock);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    frame_metrics.jpeg_decode_us = (uint32_t)(esp_timer_get_time() - decode_started_us);

    err = wait_color_transfer_locked(display, &frame_metrics.lcd_wait_us, &frame_metrics.previous_dma_us);
    if (err == ESP_OK) {
        const int64_t submit_started_us = esp_timer_get_time();
        err = submit_bitmap_locked(display, 0, display->physical_height, decoded_frame);
        frame_metrics.lcd_submit_us = (uint32_t)(esp_timer_get_time() - submit_started_us);
    }
    if (err != ESP_OK) {
        xSemaphoreGive(display->lock);
        return err;
    }

    display->next_decode_buffer = (display->next_decode_buffer + 1) % DECODE_BUFFER_COUNT;
    display->has_frame = true;
    if (metrics != NULL) {
        *metrics = frame_metrics;
    }
    xSemaphoreGive(display->lock);
    return ESP_OK;
}

bool subscreen_display_supports_frame_size(const subscreen_display_t *display,
                                           uint16_t width,
                                           uint16_t height)
{
    if (display == NULL) {
        return false;
    }
    return width == display->physical_width && height == display->physical_height;
}

bool subscreen_display_has_frame(const subscreen_display_t *display)
{
    return display != NULL && display->has_frame;
}
