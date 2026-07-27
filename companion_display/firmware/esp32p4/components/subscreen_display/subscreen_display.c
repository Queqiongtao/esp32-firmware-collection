#include "subscreen_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "companion_board.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "subscreen_display";

_Static_assert(BOARD_DISPLAY_H_RES == CONFIG_COMPANION_DISPLAY_WIDTH,
               "COMPANION_DISPLAY_WIDTH must match the panel resolution");
_Static_assert(BOARD_DISPLAY_V_RES == CONFIG_COMPANION_DISPLAY_HEIGHT,
               "COMPANION_DISPLAY_HEIGHT must match the panel resolution");

#define PANEL_FRAME_BUFFER_COUNT 2
#define JPEG_DECODE_TIMEOUT_MS 200
/* The hardware decoder emits whole MCU rows: buffers must cover dimensions
 * rounded up to 16. 1024 is already aligned; 600 decodes as 608 rows. */
#define ALIGN_UP_16(value) (((value) + 15U) & ~15U)

/* The DPI panel consumes native little-endian RGB565: UI colors and the JPEG
 * decoder output need no byte swapping (unlike the S3's SH8601 QSPI path). */
#define COLOR_BG 0x0000
#define COLOR_TRACK 0x2104
#define COLOR_TEXT 0xFFFF
#define COLOR_MUTED 0x7BEF
#define COLOR_ACCENT 0x07FF
#define COLOR_INFO 0x07FF
#define COLOR_SUCCESS 0x07E0
#define COLOR_WARNING 0xFD20
#define COLOR_DANGER 0xF800
#define COLOR_MAGENTA 0xF81F

/* Placeholder layout for the 1024x600 landscape panel. */
#define LAYOUT_MARGIN 48
#define LAYOUT_HEADER_Y 28
#define LAYOUT_HEADER_DIVIDER_Y 76
#define LAYOUT_FOOTER_HEIGHT 48
#define TEXT_SCALE_BODY 2
#define TEXT_SCALE_TITLE 3
#define TEXT_SCALE_VALUE 5

struct subscreen_display {
    esp_lcd_panel_handle_t panel;
    jpeg_decoder_handle_t jpeg_decoder;
    ppa_client_handle_t ppa_srm;
    int physical_width;
    int physical_height;
    uint8_t *decoded_frame;
    size_t decoded_frame_size;
    void *framebuffers[PANEL_FRAME_BUFFER_COUNT];
    uint8_t next_framebuffer;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t refresh_done;
    bool refresh_pending;
    int64_t refresh_submit_us;
    subscreen_transport_status_t transport_status;
    subscreen_network_status_t network_status;
    char network_ipv4[16];
    char pairing_token[7];
    bool has_frame;
};

static struct subscreen_display s_display;

static bool refresh_done_cb(esp_lcd_panel_handle_t panel,
                            esp_lcd_dpi_panel_event_data_t *edata,
                            void *user_ctx)
{
    (void)panel;
    (void)edata;
    subscreen_display_t *display = user_ctx;
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(display->refresh_done, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static esp_err_t wait_refresh_locked(subscreen_display_t *display,
                                     uint32_t *wait_us,
                                     uint32_t *refresh_us)
{
    if (!display->refresh_pending) {
        return ESP_OK;
    }

    const int64_t wait_started_us = esp_timer_get_time();
    ESP_RETURN_ON_FALSE(xSemaphoreTake(display->refresh_done, portMAX_DELAY) == pdTRUE,
                        ESP_FAIL, TAG, "wait display refresh failed");
    const int64_t completed_us = esp_timer_get_time();
    if (wait_us != NULL) {
        *wait_us = (uint32_t)(completed_us - wait_started_us);
    }
    if (refresh_us != NULL) {
        *refresh_us = (uint32_t)(completed_us - display->refresh_submit_us);
    }
    display->refresh_pending = false;
    return ESP_OK;
}

static esp_err_t submit_framebuffer_locked(subscreen_display_t *display, void *framebuffer)
{
    ESP_RETURN_ON_FALSE(!display->refresh_pending, ESP_ERR_INVALID_STATE, TAG,
                        "display refresh still pending");
    display->refresh_submit_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(display->panel, 0, 0,
                                                   display->physical_width,
                                                   display->physical_height, framebuffer),
                        TAG, "draw bitmap failed");
    /* Discard a refresh token that predates this submission. A token arriving
     * after the drain proves at least one complete scan used the new fb. */
    while (xSemaphoreTake(display->refresh_done, 0) == pdTRUE) {
    }
    display->refresh_pending = true;
    display->next_framebuffer = (display->next_framebuffer + 1) % PANEL_FRAME_BUFFER_COUNT;
    return ESP_OK;
}

static esp_err_t copy_decoded_frame_locked(subscreen_display_t *display, void *framebuffer)
{
    const ppa_srm_oper_config_t config = {
        .in = {
            .buffer = display->decoded_frame,
            .pic_w = ALIGN_UP_16(BOARD_DISPLAY_H_RES),
            .pic_h = ALIGN_UP_16(BOARD_DISPLAY_V_RES),
            .block_w = BOARD_DISPLAY_H_RES,
            .block_h = BOARD_DISPLAY_V_RES,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = framebuffer,
            .buffer_size = BOARD_DISPLAY_H_RES * BOARD_DISPLAY_V_RES * sizeof(uint16_t),
            .pic_w = BOARD_DISPLAY_H_RES,
            .pic_h = BOARD_DISPLAY_V_RES,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(display->ppa_srm, &config);
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
    draw_text(pixels, width, height, LAYOUT_MARGIN, LAYOUT_HEADER_Y, TEXT_SCALE_TITLE,
              "COMPANION", COLOR_TEXT);
    draw_text_right(pixels, width, height, width - LAYOUT_MARGIN, LAYOUT_HEADER_Y,
                    TEXT_SCALE_TITLE, "DISPLAY", COLOR_MUTED);
    fill_rect(pixels, width, height, LAYOUT_MARGIN, LAYOUT_HEADER_DIVIDER_Y,
              width - 2 * LAYOUT_MARGIN, 2, COLOR_TRACK);
}

static void draw_status_line(uint16_t *pixels, int width, int height, int y,
                             const char *text, uint16_t color)
{
    const int scale = TEXT_SCALE_TITLE;
    const int text_width_px = text_width(text, scale);
    const int total_width = 18 + text_width_px;
    const int x = (width - total_width) / 2;
    fill_circle(pixels, width, height, x + 6, y + 10, 6, color);
    draw_text(pixels, width, height, x + 18, y, scale, text, color);
}

static void draw_section_label(uint16_t *pixels, int width, int height, int y, const char *text)
{
    draw_centered_text(pixels, width, height, y, TEXT_SCALE_BODY, text, COLOR_MUTED);
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

#define PAIRING_DIGIT_WIDTH 48
#define PAIRING_DIGIT_HEIGHT 90
#define PAIRING_DIGIT_THICKNESS 6
#define PAIRING_DIGIT_GAP 12

static void draw_seven_segment_digit(uint16_t *pixels, int width, int height, int x, int y,
                                     char digit, uint16_t color)
{
    if (digit < '0' || digit > '9') return;

    const int digit_width = PAIRING_DIGIT_WIDTH;
    const int digit_height = PAIRING_DIGIT_HEIGHT;
    const int thickness = PAIRING_DIGIT_THICKNESS;
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
    const int code_width = 6 * PAIRING_DIGIT_WIDTH + 5 * PAIRING_DIGIT_GAP;
    int x = (width - code_width) / 2;
    for (int i = 0; i < 6; ++i) {
        draw_seven_segment_digit(pixels, width, height, x, y, code[i], COLOR_TEXT);
        x += PAIRING_DIGIT_WIDTH + PAIRING_DIGIT_GAP;
    }
}

static void draw_connected_status(uint16_t *pixels, int width, int height,
                                  const subscreen_display_t *display)
{
    draw_status_line(pixels, width, height, 100, "ONLINE", COLOR_SUCCESS);
    if (strlen(display->pairing_token) == 6) {
        draw_section_label(pixels, width, height, 152, "PAIR CODE");
        draw_pairing_code(pixels, width, height, 180, display->pairing_token);
        fill_rect(pixels, width, height, LAYOUT_MARGIN, 300, width - 2 * LAYOUT_MARGIN, 2, COLOR_TRACK);
        draw_section_label(pixels, width, height, 328, "DEVICE IP");
        draw_centered_text(pixels, width, height, 360, TEXT_SCALE_VALUE, display->network_ipv4, COLOR_TEXT);
        draw_centered_text(pixels, width, height, 440, TEXT_SCALE_BODY, "ENTER IP + CODE ON PC", COLOR_MUTED);
    } else {
        draw_section_label(pixels, width, height, 220, "DEVICE IP");
        draw_centered_text(pixels, width, height, 260, TEXT_SCALE_VALUE, display->network_ipv4, COLOR_TEXT);
        draw_centered_text(pixels, width, height, 360, TEXT_SCALE_BODY, "WAITING FOR PAIR CODE", COLOR_MUTED);
    }
}

static void draw_connecting_status(uint16_t *pixels, int width, int height)
{
    draw_status_line(pixels, width, height, 170, "CONNECTING", COLOR_INFO);
    draw_centered_text(pixels, width, height, 240, TEXT_SCALE_VALUE, "WIFI", COLOR_TEXT);
    fill_circle(pixels, width, height, width / 2 - 28, 340, 6, COLOR_INFO);
    fill_circle(pixels, width, height, width / 2, 340, 6, COLOR_INFO);
    fill_circle(pixels, width, height, width / 2 + 28, 340, 6, COLOR_INFO);
    draw_centered_text(pixels, width, height, 400, TEXT_SCALE_BODY, "WAITING FOR IP", COLOR_MUTED);
}

static void draw_setup_status(uint16_t *pixels, int width, int height)
{
    draw_status_line(pixels, width, height, 130, "SETUP REQUIRED", COLOR_WARNING);
    draw_section_label(pixels, width, height, 200, "JOIN WIFI NETWORK");
    draw_centered_text(pixels, width, height, 232, TEXT_SCALE_VALUE, "COMPANION AP", COLOR_TEXT);
    fill_rect(pixels, width, height, LAYOUT_MARGIN, 330, width - 2 * LAYOUT_MARGIN, 2, COLOR_TRACK);
    draw_section_label(pixels, width, height, 360, "OPEN IN BROWSER");
    draw_centered_text(pixels, width, height, 392, TEXT_SCALE_VALUE + 1, "192.168.4.1", COLOR_ACCENT);
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
    ESP_RETURN_ON_ERROR(wait_refresh_locked(display, NULL, NULL), TAG,
                        "wait previous frame failed");
    const int width = display->physical_width;
    const int height = display->physical_height;
    uint16_t *pixels = display->framebuffers[display->next_framebuffer];

    fill_rect(pixels, width, height, 0, 0, width, height, COLOR_BG);
    draw_header(pixels, width, height);
    draw_network_status(pixels, width, height, display);
    const int footer_top = height - LAYOUT_FOOTER_HEIGHT;
    const uint16_t footer_color = transport_status_color(display->transport_status);
    fill_rect(pixels, width, height, LAYOUT_MARGIN, footer_top, width - 2 * LAYOUT_MARGIN, 2, COLOR_TRACK);
    fill_circle(pixels, width, height, LAYOUT_MARGIN + 6, footer_top + LAYOUT_FOOTER_HEIGHT / 2, 5, footer_color);
    draw_text(pixels, width, height, LAYOUT_MARGIN + 22,
              footer_top + (LAYOUT_FOOTER_HEIGHT - FONT_GLYPH_HEIGHT * TEXT_SCALE_BODY) / 2,
              TEXT_SCALE_BODY, transport_status_text(display->transport_status), footer_color);

    esp_err_t err = submit_framebuffer_locked(display, pixels);
    if (err == ESP_OK) {
        err = wait_refresh_locked(display, NULL, NULL);
    }
    return err;
}

esp_err_t subscreen_display_init(subscreen_display_t **ret_display)
{
    ESP_RETURN_ON_FALSE(ret_display != NULL, ESP_ERR_INVALID_ARG, TAG, "ret_display is null");
    if (s_display.panel != NULL) {
        *ret_display = &s_display;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(board_display_init(), TAG, "board display init failed");
    ESP_RETURN_ON_ERROR(board_display_panel(&s_display.panel), TAG, "get panel failed");

    s_display.physical_width = BOARD_DISPLAY_H_RES;
    s_display.physical_height = BOARD_DISPLAY_V_RES;
    s_display.lock = xSemaphoreCreateMutex();
    s_display.refresh_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_display.lock != NULL && s_display.refresh_done != NULL, ESP_ERR_NO_MEM,
                        TAG, "display sync objects alloc failed");

    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(
                            s_display.panel, PANEL_FRAME_BUFFER_COUNT,
                            &s_display.framebuffers[0], &s_display.framebuffers[1]),
                        TAG, "get DPI framebuffers failed");
    s_display.next_framebuffer = 1;

    const ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_config, &s_display.ppa_srm), TAG,
                        "register PPA client failed");

    const jpeg_decode_engine_cfg_t engine_config = {
        .intr_priority = 0,
        .timeout_ms = JPEG_DECODE_TIMEOUT_MS,
    };
    ESP_RETURN_ON_ERROR(jpeg_new_decoder_engine(&engine_config, &s_display.jpeg_decoder), TAG,
                        "create hardware JPEG decoder failed");

    const size_t decode_size = (size_t)ALIGN_UP_16(BOARD_DISPLAY_H_RES) *
                               ALIGN_UP_16(BOARD_DISPLAY_V_RES) * sizeof(uint16_t);
    const jpeg_decode_memory_alloc_cfg_t decode_mem_config = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t allocated = 0;
    s_display.decoded_frame = jpeg_alloc_decoder_mem(decode_size, &decode_mem_config, &allocated);
    ESP_RETURN_ON_FALSE(s_display.decoded_frame != NULL, ESP_ERR_NO_MEM, TAG,
                        "decode buffer alloc failed");
    s_display.decoded_frame_size = allocated;

    const esp_lcd_dpi_panel_event_callbacks_t callbacks = {
        .on_refresh_done = refresh_done_cb,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(s_display.panel, &callbacks, &s_display),
                        TAG, "register color callback failed");

    *ret_display = &s_display;
    ESP_LOGI(TAG, "ready: physical=%dx%d hardware JPEG + PPA, decode buffer %u bytes, fbs=%p/%p",
             s_display.physical_width, s_display.physical_height,
             (unsigned)s_display.decoded_frame_size,
             s_display.framebuffers[0], s_display.framebuffers[1]);
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
    const int64_t decode_started_us = esp_timer_get_time();

    jpeg_decode_picture_info_t picture_info = {};
    esp_err_t err = jpeg_decoder_get_info(jpeg, (uint32_t)jpeg_len, &picture_info);
    if (err != ESP_OK || picture_info.width != width || picture_info.height != height) {
        ESP_LOGW(TAG, "JPEG header rejected: err=%s size=%ux%u", esp_err_to_name(err),
                 (unsigned)picture_info.width, (unsigned)picture_info.height);
        xSemaphoreGive(display->lock);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    /* IDF names RGB565 byte orders by their byte stream order. BGR produces
     * the little-endian RGB565 words consumed by this DPI panel. */
    const jpeg_decode_cfg_t decode_config = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t decoded_size = 0;
    err = jpeg_decoder_process(display->jpeg_decoder, &decode_config, jpeg, (uint32_t)jpeg_len,
                               display->decoded_frame, (uint32_t)display->decoded_frame_size, &decoded_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "JPEG decode failed: %s", esp_err_to_name(err));
        xSemaphoreGive(display->lock);
        return err;
    }
    frame_metrics.jpeg_decode_us = (uint32_t)(esp_timer_get_time() - decode_started_us);

    err = wait_refresh_locked(display, &frame_metrics.lcd_wait_us, &frame_metrics.previous_dma_us);
    if (err == ESP_OK) {
        const int64_t submit_started_us = esp_timer_get_time();
        void *framebuffer = display->framebuffers[display->next_framebuffer];
        err = copy_decoded_frame_locked(display, framebuffer);
        if (err == ESP_OK) {
            err = submit_framebuffer_locked(display, framebuffer);
        }
        frame_metrics.lcd_submit_us = (uint32_t)(esp_timer_get_time() - submit_started_us);
    }
    if (err != ESP_OK) {
        xSemaphoreGive(display->lock);
        return err;
    }

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
