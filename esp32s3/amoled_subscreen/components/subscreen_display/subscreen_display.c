#include "subscreen_display.h"

#include <stdlib.h>

#include "board_amoled_1_8.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "jpeg_decoder.h"

static const char *TAG = "subscreen_display";

#define PLACEHOLDER_CHUNK_LINES 32
#define ROTATE_CHUNK_LINES 32

#define COLOR_BG 0x1084
#define COLOR_HEADER 0x07FF
#define COLOR_ACCENT 0xFBE0
#define COLOR_TRACK 0x39E7
#define COLOR_CARD 0x2945

struct subscreen_display {
    esp_lcd_panel_handle_t panel;
    int physical_width;
    int physical_height;
    uint16_t *decoded_frame;
    uint16_t *rotate_lines;
    SemaphoreHandle_t lock;
    subscreen_transport_status_t transport_status;
    bool has_frame;
};

static struct subscreen_display s_display;

static uint16_t placeholder_pixel(int x, int y, int width, int height)
{
    if (y < 42) {
        return COLOR_HEADER;
    }
    if (y >= height - 26) {
        return COLOR_ACCENT;
    }
    if (x < 10 || x >= width - 10 || y < 52 || y >= height - 36) {
        return COLOR_TRACK;
    }
    if ((x >= 34 && x < width - 34 && y >= 92 && y < 174) ||
        (x >= 34 && x < width - 34 && y >= 208 && y < 290) ||
        (x >= 34 && x < width - 34 && y >= 324 && y < 388)) {
        return COLOR_CARD;
    }
    if ((x / 18 + y / 18) % 9 == 0) {
        return 0x2104;
    }
    return COLOR_BG;
}

static uint16_t transport_status_color(subscreen_transport_status_t status)
{
    switch (status) {
    case SUBSCREEN_TRANSPORT_WAITING_FRAME:
        return COLOR_HEADER;
    case SUBSCREEN_TRANSPORT_RECEIVING_FRAME:
        return 0x07E0;
    case SUBSCREEN_TRANSPORT_FRAME_REJECTED:
        return 0xF800;
    case SUBSCREEN_TRANSPORT_FRAME_FAILED:
        return 0xF81F;
    case SUBSCREEN_TRANSPORT_READY:
    default:
        return COLOR_ACCENT;
    }
}

static esp_err_t draw_placeholder_locked(subscreen_display_t *display)
{
    uint16_t *lines = heap_caps_malloc((size_t)display->physical_width * PLACEHOLDER_CHUNK_LINES * sizeof(uint16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(lines != NULL, ESP_ERR_NO_MEM, TAG, "placeholder buffer alloc failed");

    for (int y = 0; y < display->physical_height; y += PLACEHOLDER_CHUNK_LINES) {
        const int y2 = (y + PLACEHOLDER_CHUNK_LINES < display->physical_height) ?
                       y + PLACEHOLDER_CHUNK_LINES : display->physical_height;
        for (int row = y; row < y2; ++row) {
            uint16_t *dst = lines + (row - y) * display->physical_width;
            for (int x = 0; x < display->physical_width; ++x) {
                dst[x] = placeholder_pixel(x, row, display->physical_width, display->physical_height);
                if (row >= display->physical_height - 26) {
                    dst[x] = transport_status_color(display->transport_status);
                }
            }
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(display->panel, 0, y, display->physical_width, y2, lines);
        if (err != ESP_OK) {
            free(lines);
            return err;
        }
    }

    free(lines);
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
    ESP_RETURN_ON_ERROR(board_display_new(&s_display.panel, NULL), TAG, "get panel failed");

    s_display.physical_width = cfg->lcd_h_res;
    s_display.physical_height = cfg->lcd_v_res;
    const size_t pixel_count = (size_t)s_display.physical_width * s_display.physical_height;
    s_display.decoded_frame = heap_caps_aligned_alloc(16, pixel_count * sizeof(uint16_t),
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_display.rotate_lines = heap_caps_aligned_alloc(16,
                                                      (size_t)s_display.physical_width * ROTATE_CHUNK_LINES * sizeof(uint16_t),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_display.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_display.decoded_frame != NULL && s_display.rotate_lines != NULL && s_display.lock != NULL,
                        ESP_ERR_NO_MEM, TAG, "display buffers alloc failed");

    *ret_display = &s_display;
    ESP_LOGI(TAG, "ready: physical=%dx%d", s_display.physical_width, s_display.physical_height);
    return ESP_OK;
}

esp_err_t subscreen_display_show_placeholder(subscreen_display_t *display)
{
    ESP_RETURN_ON_FALSE(display != NULL && display->lock != NULL, ESP_ERR_INVALID_STATE, TAG, "display not ready");
    xSemaphoreTake(display->lock, portMAX_DELAY);
    display->transport_status = SUBSCREEN_TRANSPORT_READY;
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
    if (display->transport_status == status || display->has_frame) {
        xSemaphoreGive(display->lock);
        return ESP_OK;
    }
    display->transport_status = status;
    esp_err_t err = draw_placeholder_locked(display);
    xSemaphoreGive(display->lock);
    return err;
}

esp_err_t subscreen_display_draw_jpeg(subscreen_display_t *display,
                                      const uint8_t *jpeg,
                                      size_t jpeg_len,
                                      uint16_t width,
                                      uint16_t height)
{
    ESP_RETURN_ON_FALSE(display != NULL && jpeg != NULL && jpeg_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid JPEG");
    ESP_RETURN_ON_FALSE(subscreen_display_supports_frame_size(display, width, height), ESP_ERR_INVALID_SIZE, TAG,
                        "unexpected frame size %ux%u", width, height);

    xSemaphoreTake(display->lock, portMAX_DELAY);
    esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = jpeg_len,
        .outbuf = (uint8_t *)display->decoded_frame,
        .outbuf_size = (size_t)display->physical_width * display->physical_height * sizeof(uint16_t),
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = {.swap_color_bytes = 0},
    };
    esp_jpeg_image_output_t output = {};
    esp_err_t err = esp_jpeg_decode(&jpeg_cfg, &output);
    if (err != ESP_OK || output.width != width || output.height != height) {
        ESP_LOGW(TAG, "JPEG decode failed: err=%s size=%ux%u", esp_err_to_name(err), output.width, output.height);
        xSemaphoreGive(display->lock);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }

    if (width == display->physical_width && height == display->physical_height) {
        for (int dest_y = 0; dest_y < display->physical_height; dest_y += ROTATE_CHUNK_LINES) {
            const int dest_y2 = (dest_y + ROTATE_CHUNK_LINES < display->physical_height) ?
                               dest_y + ROTATE_CHUNK_LINES : display->physical_height;
            err = esp_lcd_panel_draw_bitmap(display->panel, 0, dest_y, display->physical_width, dest_y2,
                                            display->decoded_frame + dest_y * display->physical_width);
            if (err != ESP_OK) {
                xSemaphoreGive(display->lock);
                return err;
            }
        }
    } else {
    for (int dest_y = 0; dest_y < display->physical_height; dest_y += ROTATE_CHUNK_LINES) {
        const int dest_y2 = (dest_y + ROTATE_CHUNK_LINES < display->physical_height) ?
                           dest_y + ROTATE_CHUNK_LINES : display->physical_height;
        for (int y = dest_y; y < dest_y2; ++y) {
            uint16_t *dst = display->rotate_lines + (y - dest_y) * display->physical_width;
            for (int x = 0; x < display->physical_width; ++x) {
                dst[x] = display->decoded_frame[(display->physical_width - 1 - x) * width + y];
            }
        }
        err = esp_lcd_panel_draw_bitmap(display->panel, 0, dest_y, display->physical_width, dest_y2,
                                        display->rotate_lines);
        if (err != ESP_OK) {
            xSemaphoreGive(display->lock);
            return err;
        }
    }
    }

    display->has_frame = true;
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
    return (width == display->physical_width && height == display->physical_height) ||
           (width == display->physical_height && height == display->physical_width);
}

bool subscreen_display_has_frame(const subscreen_display_t *display)
{
    return display != NULL && display->has_frame;
}
