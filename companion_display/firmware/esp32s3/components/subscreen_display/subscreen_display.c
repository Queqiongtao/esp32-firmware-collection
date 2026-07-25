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

#define COLOR_BG SH8601_RGB565(0x0000)
#define COLOR_TRACK SH8601_RGB565(0x2104)
#define COLOR_TEXT SH8601_RGB565(0xFFFF)
#define COLOR_MUTED SH8601_RGB565(0x7BEF)
#define COLOR_ACCENT SH8601_RGB565(0x07FF)
#define COLOR_INFO SH8601_RGB565(0x07FF)
#define COLOR_SUCCESS SH8601_RGB565(0x07E0)
#define COLOR_WARNING SH8601_RGB565(0xFD20)
#define COLOR_DANGER SH8601_RGB565(0xF800)
#define COLOR_MAGENTA SH8601_RGB565(0xF81F)

#define LAYOUT_MARGIN 24
#define LAYOUT_HEADER_Y 20
#define LAYOUT_HEADER_DIVIDER_Y 54
#define LAYOUT_FOOTER_HEIGHT 34

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

#define FONT_GLYPH_HEIGHT 7

static void draw_text_right(uint16_t *pixels, int width, int height, int right, int y,
                            int scale, const char *text, uint16_t color)
{
    draw_text(pixels, width, height, right - text_width(text, scale), y, scale, text, color);
}

static void fill_circle(uint16_t *pixels, int width, int height, int center_x, int center_y,
                        int radius, uint16_t color)
{
    const int radius_squared = radius * radius;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius_squared) {
                const int px = center_x + x;
                const int py = center_y + y;
                if (px >= 0 && px < width && py >= 0 && py < height) pixels[py * width + px] = color;
            }
        }
    }
}

static void draw_header(uint16_t *pixels, int width, int height)
{
    draw_text(pixels, width, height, LAYOUT_MARGIN, LAYOUT_HEADER_Y, 2, "COMPANION", COLOR_TEXT);
    draw_text_right(pixels, width, height, width - LAYOUT_MARGIN, LAYOUT_HEADER_Y, 2, "DISPLAY", COLOR_MUTED);
    fill_rect(pixels, width, height, LAYOUT_MARGIN, LAYOUT_HEADER_DIVIDER_Y,
              width - 2 * LAYOUT_MARGIN, 1, COLOR_TRACK);
}

static void draw_status_line(uint16_t *pixels, int width, int height, int y,
                             const char *text, uint16_t color)
{
    const int scale = 2;
    const int text_width_px = text_width(text, scale);
    const int total_width = 10 + text_width_px;
    const int x = (width - total_width) / 2;
    fill_circle(pixels, width, height, x + 4, y + 7, 4, color);
    draw_text(pixels, width, height, x + 10, y, scale, text, color);
}

static void draw_section_label(uint16_t *pixels, int width, int height, int y, const char *text)
{
    draw_centered_text(pixels, width, height, y, 2, text, COLOR_MUTED);
}

enum {
    SEGMENT_A = 1 << 0,
    SEGMENT_B = 1 << 1,
    SEGMENT_C = 1 << 2,
    SEGMENT_D = 1 << 3,
    SEGMENT_E = 1 << 4,
    SEGMENT_F = 1 << 5,
    SEGMENT_G = 1 << 6,
};

static const uint8_t s_digit_segments[10] = {
    SEGMENT_A | SEGMENT_B | SEGMENT_C | SEGMENT_D | SEGMENT_E | SEGMENT_F,
    SEGMENT_B | SEGMENT_C,
    SEGMENT_A | SEGMENT_B | SEGMENT_D | SEGMENT_E | SEGMENT_G,
    SEGMENT_A | SEGMENT_B | SEGMENT_C | SEGMENT_D | SEGMENT_G,
    SEGMENT_B | SEGMENT_C | SEGMENT_F | SEGMENT_G,
    SEGMENT_A | SEGMENT_C | SEGMENT_D | SEGMENT_F | SEGMENT_G,
    SEGMENT_A | SEGMENT_C | SEGMENT_D | SEGMENT_E | SEGMENT_F | SEGMENT_G,
    SEGMENT_A | SEGMENT_B | SEGMENT_C,
    SEGMENT_A | SEGMENT_B | SEGMENT_C | SEGMENT_D | SEGMENT_E | SEGMENT_F | SEGMENT_G,
    SEGMENT_A | SEGMENT_B | SEGMENT_C | SEGMENT_D | SEGMENT_F | SEGMENT_G,
};

static void draw_seven_segment_digit(uint16_t *pixels, int width, int height, int x, int y,
                                     char digit, uint16_t color)
{
    if (digit < '0' || digit > '9') return;

    const int digit_width = 32;
    const int digit_height = 62;
    const int thickness = 4;
    const int middle_y = y + digit_height / 2 - thickness / 2;
    const uint8_t segments = s_digit_segments[digit - '0'];
    if (segments & SEGMENT_A) fill_rect(pixels, width, height, x + thickness, y, digit_width - 2 * thickness, thickness, color);
    if (segments & SEGMENT_B) fill_rect(pixels, width, height, x + digit_width - thickness, y + thickness, thickness, digit_height / 2 - thickness, color);
    if (segments & SEGMENT_C) fill_rect(pixels, width, height, x + digit_width - thickness, middle_y + thickness, thickness, digit_height / 2 - thickness, color);
    if (segments & SEGMENT_D) fill_rect(pixels, width, height, x + thickness, y + digit_height - thickness, digit_width - 2 * thickness, thickness, color);
    if (segments & SEGMENT_E) fill_rect(pixels, width, height, x, middle_y + thickness, thickness, digit_height / 2 - thickness, color);
    if (segments & SEGMENT_F) fill_rect(pixels, width, height, x, y + thickness, thickness, digit_height / 2 - thickness, color);
    if (segments & SEGMENT_G) fill_rect(pixels, width, height, x + thickness, middle_y, digit_width - 2 * thickness, thickness, color);
}

static void draw_pairing_code(uint16_t *pixels, int width, int height, int y, const char *code)
{
    const int digit_width = 32;
    const int gap = 8;
    const int code_width = 6 * digit_width + 5 * gap;
    int x = (width - code_width) / 2;
    for (int i = 0; i < 6; ++i) {
        draw_seven_segment_digit(pixels, width, height, x, y, code[i], COLOR_TEXT);
        x += digit_width + gap;
    }
}

static void draw_connected_status(uint16_t *pixels, int width, int height,
                                  const subscreen_display_t *display)
{
    draw_status_line(pixels, width, height, 74, "ONLINE", COLOR_SUCCESS);
    if (strlen(display->pairing_token) == 6) {
        draw_section_label(pixels, width, height, 112, "PAIR CODE");
        draw_pairing_code(pixels, width, height, 138, display->pairing_token);
        fill_rect(pixels, width, height, LAYOUT_MARGIN, 224, width - 2 * LAYOUT_MARGIN, 1, COLOR_TRACK);
        draw_section_label(pixels, width, height, 246, "DEVICE IP");
        draw_centered_text(pixels, width, height, 272, 3, display->network_ipv4, COLOR_TEXT);
        draw_centered_text(pixels, width, height, 322, 2, "ENTER IP + CODE ON PC", COLOR_MUTED);
    } else {
        draw_section_label(pixels, width, height, 154, "DEVICE IP");
        draw_centered_text(pixels, width, height, 182, 3, display->network_ipv4, COLOR_TEXT);
        draw_centered_text(pixels, width, height, 240, 2, "WAITING FOR PAIR CODE", COLOR_MUTED);
    }
}

static void draw_connecting_status(uint16_t *pixels, int width, int height)
{
    draw_status_line(pixels, width, height, 136, "CONNECTING", COLOR_INFO);
    draw_centered_text(pixels, width, height, 184, 3, "WIFI", COLOR_TEXT);
    fill_circle(pixels, width, height, width / 2 - 16, 236, 4, COLOR_INFO);
    fill_circle(pixels, width, height, width / 2, 236, 4, COLOR_INFO);
    fill_circle(pixels, width, height, width / 2 + 16, 236, 4, COLOR_INFO);
    draw_centered_text(pixels, width, height, 274, 2, "WAITING FOR IP", COLOR_MUTED);
}

static void draw_setup_status(uint16_t *pixels, int width, int height)
{
    draw_status_line(pixels, width, height, 104, "SETUP REQUIRED", COLOR_WARNING);
    draw_section_label(pixels, width, height, 158, "JOIN WIFI NETWORK");
    draw_centered_text(pixels, width, height, 188, 3, "COMPANION AP", COLOR_TEXT);
    fill_rect(pixels, width, height, LAYOUT_MARGIN, 236, width - 2 * LAYOUT_MARGIN, 1, COLOR_TRACK);
    draw_section_label(pixels, width, height, 260, "OPEN IN BROWSER");
    draw_centered_text(pixels, width, height, 290, 4, "192.168.4.1", COLOR_ACCENT);
}

static void draw_network_status(uint16_t *pixels, int width, int height,
                                const subscreen_display_t *display)
{
    switch (display->network_status) {
    case SUBSCREEN_NETWORK_CONNECTED:
        draw_connected_status(pixels, width, height, display);
        break;
    case SUBSCREEN_NETWORK_CONNECTING:
        draw_connecting_status(pixels, width, height);
        break;
    case SUBSCREEN_NETWORK_SETUP_REQUIRED:
    default:
        draw_setup_status(pixels, width, height);
        break;
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
    draw_header(pixels, width, height);
    draw_network_status(pixels, width, height, display);
    const int footer_top = height - LAYOUT_FOOTER_HEIGHT;
    const uint16_t footer_color = transport_status_color(display->transport_status);
    fill_rect(pixels, width, height, LAYOUT_MARGIN, footer_top, width - 2 * LAYOUT_MARGIN, 1, COLOR_TRACK);
    fill_circle(pixels, width, height, LAYOUT_MARGIN + 4, footer_top + LAYOUT_FOOTER_HEIGHT / 2, 3, footer_color);
    draw_text(pixels, width, height, LAYOUT_MARGIN + 14,
              footer_top + (LAYOUT_FOOTER_HEIGHT - FONT_GLYPH_HEIGHT * 2) / 2,
              2, transport_status_text(display->transport_status), footer_color);

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
