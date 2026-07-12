#include "frame_pipeline.h"

#include <string.h>
#include "esp_check.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "subscreen_display.h"

static const char *TAG = "frame_pipeline";
#define FRAME_LIMIT ((size_t)CONFIG_COMPANION_FRAME_LIMIT_KB * 1024U)

typedef struct { uint8_t *data; size_t length; uint16_t width; uint16_t height; uint32_t sequence; int64_t received_us; } frame_t;
static struct {
    subscreen_display_t *display;
    QueueHandle_t empty;
    QueueHandle_t filled;
    frame_t *frames;
    companion_transport_t active;
    uint32_t replaced_frames;
    frame_pipeline_presented_callback_t presented_callback;
    void *presented_context;
    portMUX_TYPE lock;
} s_pipeline = {.lock = portMUX_INITIALIZER_UNLOCKED};

static const char *transport_name(companion_transport_t source)
{
    switch (source) {
    case COMPANION_TRANSPORT_TCP_VIDEO: return "tcp";
    case COMPANION_TRANSPORT_UDP_VIDEO: return "udp";
    case COMPANION_TRANSPORT_NONE: default: return "none";
    }
}

static void return_frame(frame_t *frame)
{
    if (frame != NULL) xQueueSend(s_pipeline.empty, &frame, 0);
}

static void draw_task(void *arg)
{
    (void)arg;
    while (true) {
        frame_t *frame = NULL;
        if (xQueueReceive(s_pipeline.filled, &frame, portMAX_DELAY) != pdPASS) continue;
        frame_t *newer = NULL;
        while (xQueueReceive(s_pipeline.filled, &newer, 0) == pdPASS) {
            return_frame(frame);
            frame = newer;
        }
        const uint32_t queue_wait_us = (uint32_t)(esp_timer_get_time() - frame->received_us);
        subscreen_display_frame_metrics_t metrics = {};
        esp_err_t err = subscreen_display_draw_jpeg(s_pipeline.display, frame->data, frame->length,
                                                     frame->width, frame->height, &metrics);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "JPEG frame rejected by display: %s", esp_err_to_name(err));
            subscreen_display_set_transport_status(s_pipeline.display, SUBSCREEN_TRANSPORT_FRAME_FAILED);
        }
        if (err == ESP_OK && s_pipeline.presented_callback != NULL) {
            frame_pipeline_presented_frame_t presented = {
                .sequence = frame->sequence,
                .queue_wait_us = queue_wait_us,
                .jpeg_decode_us = metrics.jpeg_decode_us,
                .lcd_wait_us = metrics.lcd_wait_us,
                .lcd_submit_us = metrics.lcd_submit_us,
                .previous_dma_us = metrics.previous_dma_us,
                .replaced_frames = s_pipeline.replaced_frames,
            };
            s_pipeline.presented_callback(&presented, s_pipeline.presented_context);
        }
        return_frame(frame);
    }
}

bool frame_pipeline_acquire(companion_transport_t source)
{
    bool acquired = false;
    portENTER_CRITICAL(&s_pipeline.lock);
    if (s_pipeline.active == COMPANION_TRANSPORT_NONE || s_pipeline.active == source) {
        s_pipeline.active = source;
        acquired = true;
    }
    portEXIT_CRITICAL(&s_pipeline.lock);
    return acquired;
}

void frame_pipeline_release(companion_transport_t source)
{
    portENTER_CRITICAL(&s_pipeline.lock);
    if (s_pipeline.active == source) s_pipeline.active = COMPANION_TRANSPORT_NONE;
    portEXIT_CRITICAL(&s_pipeline.lock);
}

companion_transport_t frame_pipeline_active_transport(void)
{
    companion_transport_t active;
    portENTER_CRITICAL(&s_pipeline.lock);
    active = s_pipeline.active;
    portEXIT_CRITICAL(&s_pipeline.lock);
    return active;
}

void frame_pipeline_set_presented_callback(frame_pipeline_presented_callback_t callback, void *context)
{
    portENTER_CRITICAL(&s_pipeline.lock);
    s_pipeline.presented_callback = callback;
    s_pipeline.presented_context = context;
    portEXIT_CRITICAL(&s_pipeline.lock);
}

esp_err_t frame_pipeline_submit_result_to_esp_err(frame_pipeline_submit_result_t result)
{
    switch (result) {
    case FRAME_PIPELINE_SUBMIT_OK:
        return ESP_OK;
    case FRAME_PIPELINE_SUBMIT_INVALID_ARG:
    case FRAME_PIPELINE_SUBMIT_BAD_HEADER:
        return ESP_ERR_INVALID_ARG;
    case FRAME_PIPELINE_SUBMIT_BUSY:
        return ESP_ERR_INVALID_STATE;
    case FRAME_PIPELINE_SUBMIT_BAD_CRC:
        return ESP_ERR_INVALID_CRC;
    case FRAME_PIPELINE_SUBMIT_NO_BUFFER:
    default:
        return ESP_ERR_NO_MEM;
    }
}

const char *frame_pipeline_submit_result_name(frame_pipeline_submit_result_t result)
{
    switch (result) {
    case FRAME_PIPELINE_SUBMIT_OK:
        return "ok";
    case FRAME_PIPELINE_SUBMIT_INVALID_ARG:
        return "invalid argument";
    case FRAME_PIPELINE_SUBMIT_BUSY:
        return "transport busy";
    case FRAME_PIPELINE_SUBMIT_BAD_HEADER:
        return "bad frame header";
    case FRAME_PIPELINE_SUBMIT_BAD_CRC:
        return "bad frame CRC";
    case FRAME_PIPELINE_SUBMIT_NO_BUFFER:
        return "no frame buffer";
    default:
        return "unknown submit result";
    }
}

frame_pipeline_submit_result_t frame_pipeline_submit(companion_transport_t source, const companion_frame_header_t *header,
                                                     const uint8_t *payload)
{
    if (header == NULL || payload == NULL) {
        ESP_LOGW(TAG, "empty frame: transport=%s", transport_name(source));
        return FRAME_PIPELINE_SUBMIT_INVALID_ARG;
    }
    if (!frame_pipeline_acquire(source)) {
        ESP_LOGW(TAG, "transport busy: source=%s active=%s", transport_name(source), transport_name(frame_pipeline_active_transport()));
        return FRAME_PIPELINE_SUBMIT_BUSY;
    }
    if (header->magic != COMPANION_FRAME_MAGIC || header->version != COMPANION_FRAME_VERSION ||
        header->format != COMPANION_FRAME_FORMAT_JPEG || header->header_size != sizeof(*header) ||
        header->payload_size == 0 || header->payload_size > FRAME_LIMIT) {
        ESP_LOGW(TAG, "bad frame header: transport=%s seq=%lu magic=%08lx version=%u format=%u payload=%lu",
                 transport_name(source), (unsigned long)header->sequence, (unsigned long)header->magic,
                 header->version, header->format, (unsigned long)header->payload_size);
        return FRAME_PIPELINE_SUBMIT_BAD_HEADER;
    }
    if (esp_crc32_le(0, payload, header->payload_size) != header->crc32) {
        ESP_LOGW(TAG, "bad frame CRC: transport=%s seq=%lu payload=%lu", transport_name(source),
                 (unsigned long)header->sequence, (unsigned long)header->payload_size);
        return FRAME_PIPELINE_SUBMIT_BAD_CRC;
    }
    frame_t *frame = NULL;
    frame_t *replaced = NULL;
    if (xQueueReceive(s_pipeline.empty, &frame, 0) != pdPASS) {
        if (xQueueReceive(s_pipeline.filled, &replaced, 0) != pdPASS) {
            ESP_LOGW(TAG, "no frame buffer: transport=%s seq=%lu", transport_name(source), (unsigned long)header->sequence);
            return FRAME_PIPELINE_SUBMIT_NO_BUFFER;
        }
        frame = replaced;
        s_pipeline.replaced_frames++;
    }
    memcpy(frame->data, payload, header->payload_size);
    frame->length = header->payload_size;
    frame->width = header->width;
    frame->height = header->height;
    frame->sequence = header->sequence;
    frame->received_us = esp_timer_get_time();
    if (xQueueReceive(s_pipeline.filled, &replaced, 0) == pdPASS) {
        s_pipeline.replaced_frames++;
        return_frame(replaced);
    }
    if (xQueueSend(s_pipeline.filled, &frame, 0) != pdPASS) {
        return_frame(frame);
        ESP_LOGW(TAG, "frame queue full: transport=%s seq=%lu", transport_name(source), (unsigned long)header->sequence);
        return FRAME_PIPELINE_SUBMIT_NO_BUFFER;
    }
    return FRAME_PIPELINE_SUBMIT_OK;
}

esp_err_t frame_pipeline_init(subscreen_display_t *display)
{
    ESP_RETURN_ON_FALSE(display != NULL, ESP_ERR_INVALID_ARG, TAG, "display is null");
    const size_t count = CONFIG_COMPANION_FRAME_BUFFER_COUNT;
    s_pipeline.display = display;
    s_pipeline.empty = xQueueCreate(count, sizeof(frame_t *));
    s_pipeline.filled = xQueueCreate(1, sizeof(frame_t *));
    s_pipeline.frames = heap_caps_calloc(count, sizeof(frame_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_pipeline.empty && s_pipeline.filled && s_pipeline.frames, ESP_ERR_NO_MEM, TAG, "queue alloc");
    for (size_t i = 0; i < count; ++i) {
        frame_t *frame = &s_pipeline.frames[i];
        frame->data = heap_caps_aligned_alloc(16, FRAME_LIMIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(frame->data != NULL, ESP_ERR_NO_MEM, TAG, "PSRAM frame alloc");
        xQueueSend(s_pipeline.empty, &frame, 0);
    }
    ESP_RETURN_ON_FALSE(xTaskCreate(draw_task, "frame_draw", 6144, NULL,
                                    CONFIG_COMPANION_DRAW_TASK_PRIORITY, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "draw task");
    ESP_LOGI(TAG, "ready: buffers=%u limit=%uKiB", (unsigned)count, CONFIG_COMPANION_FRAME_LIMIT_KB);
    return ESP_OK;
}
