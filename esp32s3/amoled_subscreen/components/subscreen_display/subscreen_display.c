#include "subscreen_display.h"

#include <stdlib.h>

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

#define COLOR_BG 0x1084
#define COLOR_HEADER 0x07FF
#define COLOR_ACCENT 0xFBE0
#define COLOR_TRACK 0x39E7
#define COLOR_CARD 0x2945

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
    ESP_RETURN_ON_ERROR(wait_color_transfer_locked(display, NULL, NULL), TAG,
                        "wait previous frame failed");
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
        esp_err_t err = submit_bitmap_locked(display, y, y2, lines);
        if (err == ESP_OK) {
            err = wait_color_transfer_locked(display, NULL, NULL);
        }
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
