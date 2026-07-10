#include "usb_extend_screen.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tusb.h"
#include "usb_descriptors.h"

static const char *TAG = "usb_extend_screen";

#define FRAME_LIMIT_BYTES ((size_t)CONFIG_USB_EXTEND_SCREEN_FRAME_LIMIT_KB * 1024U)
#define STATS_INTERVAL_US (5LL * 1000 * 1000)

enum {
    UDISP_TYPE_RGB565 = 0,
    UDISP_TYPE_RGB888 = 1,
    UDISP_TYPE_YUV420 = 2,
    UDISP_TYPE_JPG = 3,
    UDISP_TYPE_END = 0xff,
};

typedef struct __attribute__((packed)) {
    uint16_t crc16;
    uint8_t type;
    uint8_t cmd;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t frame_id : 10;
    uint32_t payload_total : 22;
} udisp_frame_header_t;

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t received;
    size_t total;
    uint16_t width;
    uint16_t height;
    int64_t started_us;
} frame_t;

typedef struct {
    uint64_t rx_bytes;
    uint64_t jpeg_bytes;
    uint64_t decode_us;
    uint64_t lcd_wait_us;
    uint64_t lcd_dma_us;
    uint64_t lcd_submit_us;
    uint64_t latency_us;
    uint32_t rx_callbacks;
    uint32_t accepted_frames;
    uint32_t completed_frames;
    uint32_t rejected_frames;
    uint32_t no_free_frames;
    uint32_t stale_frames;
    uint32_t presented_frames;
    uint32_t failed_frames;
    uint32_t queue_peak;
    uint32_t decode_max_us;
    uint32_t lcd_wait_max_us;
    uint32_t lcd_dma_max_us;
    uint32_t lcd_dma_samples;
    uint32_t latency_max_us;
} performance_stats_t;

static struct {
    subscreen_display_t *display;
    usb_phy_handle_t phy;
    QueueHandle_t empty_frames;
    QueueHandle_t filled_frames;
    frame_t *frames;
    frame_t *current_frame;
    uint32_t skip_remaining;
    uint8_t header_data[sizeof(udisp_frame_header_t)];
    size_t header_received;
    performance_stats_t stats;
    int64_t stats_window_started_us;
    bool initialized;
} s_usb;

static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;

static void stats_record_rx(size_t bytes)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_usb.stats.rx_callbacks++;
    s_usb.stats.rx_bytes += bytes;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void stats_record_rejected(bool no_free)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_usb.stats.rejected_frames++;
    if (no_free) {
        s_usb.stats.no_free_frames++;
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

static void stats_record_completed(size_t jpeg_bytes, uint32_t queue_depth)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_usb.stats.completed_frames++;
    s_usb.stats.jpeg_bytes += jpeg_bytes;
    if (queue_depth > s_usb.stats.queue_peak) {
        s_usb.stats.queue_peak = queue_depth;
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

static void stats_record_stale(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_usb.stats.stale_frames++;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void stats_record_display(const subscreen_display_frame_metrics_t *metrics,
                                 uint32_t latency_us,
                                 bool success)
{
    portENTER_CRITICAL(&s_stats_lock);
    if (success) {
        s_usb.stats.presented_frames++;
        s_usb.stats.decode_us += metrics->jpeg_decode_us;
        s_usb.stats.lcd_wait_us += metrics->lcd_wait_us;
        s_usb.stats.lcd_submit_us += metrics->lcd_submit_us;
        s_usb.stats.latency_us += latency_us;
        if (metrics->previous_dma_us > 0) {
            s_usb.stats.lcd_dma_us += metrics->previous_dma_us;
            s_usb.stats.lcd_dma_samples++;
            if (metrics->previous_dma_us > s_usb.stats.lcd_dma_max_us) {
                s_usb.stats.lcd_dma_max_us = metrics->previous_dma_us;
            }
        }
        if (metrics->jpeg_decode_us > s_usb.stats.decode_max_us) {
            s_usb.stats.decode_max_us = metrics->jpeg_decode_us;
        }
        if (metrics->lcd_wait_us > s_usb.stats.lcd_wait_max_us) {
            s_usb.stats.lcd_wait_max_us = metrics->lcd_wait_us;
        }
        if (latency_us > s_usb.stats.latency_max_us) {
            s_usb.stats.latency_max_us = latency_us;
        }
    } else {
        s_usb.stats.failed_frames++;
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

static void report_stats_if_due(void)
{
    const int64_t now_us = esp_timer_get_time();
    const int64_t elapsed_us = now_us - s_usb.stats_window_started_us;
    if (elapsed_us < STATS_INTERVAL_US) {
        return;
    }

    performance_stats_t stats;
    portENTER_CRITICAL(&s_stats_lock);
    stats = s_usb.stats;
    memset(&s_usb.stats, 0, sizeof(s_usb.stats));
    s_usb.stats_window_started_us = now_us;
    portEXIT_CRITICAL(&s_stats_lock);

    const uint32_t rx_kbps = elapsed_us > 0 ?
                             (uint32_t)(stats.rx_bytes * 8U * 1000U / (uint64_t)elapsed_us) : 0;
    const uint32_t fps_x10 = elapsed_us > 0 ?
                             (uint32_t)((uint64_t)stats.presented_frames * 10U * 1000000U /
                                        (uint64_t)elapsed_us) : 0;
    const uint32_t shown = stats.presented_frames;
    const uint32_t jpeg_avg = stats.completed_frames > 0 ?
                              (uint32_t)(stats.jpeg_bytes / stats.completed_frames) : 0;
    const uint32_t decode_avg = shown > 0 ? (uint32_t)(stats.decode_us / shown) : 0;
    const uint32_t lcd_wait_avg = shown > 0 ? (uint32_t)(stats.lcd_wait_us / shown) : 0;
    const uint32_t lcd_dma_avg = stats.lcd_dma_samples > 0 ?
                                 (uint32_t)(stats.lcd_dma_us / stats.lcd_dma_samples) : 0;
    const uint32_t lcd_submit_avg = shown > 0 ? (uint32_t)(stats.lcd_submit_us / shown) : 0;
    const uint32_t latency_avg = shown > 0 ? (uint32_t)(stats.latency_us / shown) : 0;

    ESP_LOGI(TAG,
             "perf: rx=%" PRIu32 "kbps callbacks=%" PRIu32 " accepted=%" PRIu32
             " complete=%" PRIu32 " shown=%" PRIu32 " fps=%" PRIu32 ".%" PRIu32
             " rejected=%" PRIu32 " stale=%" PRIu32 " failed=%" PRIu32 " qpeak=%" PRIu32,
             rx_kbps, stats.rx_callbacks, stats.accepted_frames, stats.completed_frames,
             shown, fps_x10 / 10, fps_x10 % 10, stats.rejected_frames,
             stats.stale_frames, stats.failed_frames, stats.queue_peak);
    ESP_LOGI(TAG,
             "perf-time: jpeg=%" PRIu32 "B decode=%" PRIu32 "/%" PRIu32
             "us lcd-dma=%" PRIu32 "/%" PRIu32 "us wait=%" PRIu32 "/%" PRIu32
             "us submit=%" PRIu32
             "us latency=%" PRIu32 "/%" PRIu32 "us no-free=%" PRIu32,
             jpeg_avg, decode_avg, stats.decode_max_us, lcd_dma_avg, stats.lcd_dma_max_us,
             lcd_wait_avg, stats.lcd_wait_max_us, lcd_submit_avg, latency_avg,
             stats.latency_max_us, stats.no_free_frames);
}

static void return_empty(frame_t *frame)
{
    if (frame == NULL) {
        return;
    }
    frame->received = 0;
    frame->total = 0;
    if (xQueueSend(s_usb.empty_frames, &frame, 0) != pdPASS) {
        ESP_LOGW(TAG, "empty frame queue full");
    }
}

static void show_transport_status(subscreen_transport_status_t status)
{
    esp_err_t err = subscreen_display_set_transport_status(s_usb.display, status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "update transport status failed: %s", esp_err_to_name(err));
    }
}

static void return_current_frame_to_empty(void)
{
    if (s_usb.current_frame != NULL) {
        return_empty(s_usb.current_frame);
        s_usb.current_frame = NULL;
    }
}

static void begin_frame_from_header(void)
{
    const udisp_frame_header_t *header = (const udisp_frame_header_t *)s_usb.header_data;
    s_usb.header_received = 0;

    if (header->type == UDISP_TYPE_END) {
        return;
    }

    if (header->type != UDISP_TYPE_JPG || header->x != 0 || header->y != 0 ||
        !subscreen_display_supports_frame_size(s_usb.display, header->width, header->height) ||
        header->payload_total == 0 || header->payload_total > FRAME_LIMIT_BYTES) {
        stats_record_rejected(false);
        s_usb.skip_remaining = header->payload_total;
        ESP_LOGW(TAG, "drop header: type=%u area=%u,%u %ux%u payload=%" PRIu32,
                 header->type, header->x, header->y, header->width, header->height,
                 (uint32_t)header->payload_total);
        show_transport_status(SUBSCREEN_TRANSPORT_FRAME_REJECTED);
        return;
    }

    frame_t *frame = NULL;
    if (xQueueReceive(s_usb.empty_frames, &frame, 0) != pdPASS) {
        stats_record_rejected(true);
        s_usb.skip_remaining = header->payload_total;
        ESP_LOGW(TAG, "drop frame %u: no free buffer", header->frame_id);
        show_transport_status(SUBSCREEN_TRANSPORT_FRAME_REJECTED);
        return;
    }
    frame->received = 0;
    frame->total = header->payload_total;
    frame->width = header->width;
    frame->height = header->height;
    frame->started_us = esp_timer_get_time();
    s_usb.current_frame = frame;
    portENTER_CRITICAL(&s_stats_lock);
    s_usb.stats.accepted_frames++;
    portEXIT_CRITICAL(&s_stats_lock);
    show_transport_status(SUBSCREEN_TRANSPORT_WAITING_FRAME);
}

static void complete_current_frame(void)
{
    frame_t *frame = s_usb.current_frame;
    if (xQueueSend(s_usb.filled_frames, &frame, 0) != pdPASS) {
        ESP_LOGW(TAG, "filled frame queue full");
        return_current_frame_to_empty();
        stats_record_rejected(false);
        show_transport_status(SUBSCREEN_TRANSPORT_FRAME_REJECTED);
        return;
    }
    s_usb.current_frame = NULL;
    stats_record_completed(frame->received, uxQueueMessagesWaiting(s_usb.filled_frames));
}

void tud_vendor_rx_cb(uint8_t itf, const uint8_t *buffer, uint16_t size)
{
    (void)buffer;
    (void)size;
    uint8_t discard_buffer[CFG_TUD_VENDOR_EPSIZE];
    size_t callback_bytes = 0;
    while (tud_vendor_n_available(itf) > 0) {
        const uint32_t available = tud_vendor_n_available(itf);
        uint32_t read = 0;

        if (s_usb.skip_remaining > 0) {
            uint32_t read_len = available < s_usb.skip_remaining ? available : s_usb.skip_remaining;
            if (read_len > sizeof(discard_buffer)) {
                read_len = sizeof(discard_buffer);
            }
            read = tud_vendor_n_read(itf, discard_buffer, read_len);
            s_usb.skip_remaining -= read;
        } else if (s_usb.current_frame != NULL) {
            frame_t *frame = s_usb.current_frame;
            const size_t remaining = frame->total - frame->received;
            const uint32_t read_len = available < remaining ? available : (uint32_t)remaining;
            read = tud_vendor_n_read(itf, frame->data + frame->received, read_len);
            frame->received += read;
            if (frame->received == frame->total) {
                complete_current_frame();
            }
        } else {
            const size_t remaining = sizeof(s_usb.header_data) - s_usb.header_received;
            const uint32_t read_len = available < remaining ? available : (uint32_t)remaining;
            read = tud_vendor_n_read(itf, s_usb.header_data + s_usb.header_received, read_len);
            s_usb.header_received += read;
            if (s_usb.header_received == sizeof(s_usb.header_data)) {
                begin_frame_from_header();
            }
        }

        if (read == 0) {
            break;
        }
        callback_bytes += read;
        show_transport_status(SUBSCREEN_TRANSPORT_RECEIVING_FRAME);
    }
    stats_record_rx(callback_bytes);
}

static void transfer_task(void *arg)
{
    (void)arg;
    while (true) {
        frame_t *frame = NULL;
        if (xQueueReceive(s_usb.filled_frames, &frame, portMAX_DELAY) != pdPASS) {
            continue;
        }
        frame_t *newer = NULL;
        while (xQueueReceive(s_usb.filled_frames, &newer, 0) == pdPASS) {
            return_empty(frame);
            stats_record_stale();
            frame = newer;
        }

        subscreen_display_frame_metrics_t metrics = {};
        esp_err_t err = subscreen_display_draw_jpeg(s_usb.display, frame->data, frame->received,
                                                    frame->width, frame->height, &metrics);
        const int64_t now_us = esp_timer_get_time();
        const uint32_t latency_us = now_us > frame->started_us ?
                                    (uint32_t)(now_us - frame->started_us) : 0;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "frame draw failed: %s", esp_err_to_name(err));
            show_transport_status(SUBSCREEN_TRANSPORT_FRAME_FAILED);
        }
        stats_record_display(&metrics, latency_us, err == ESP_OK);
        return_empty(frame);
        report_stats_if_due();
    }
}

static esp_err_t allocate_frames(void)
{
    const size_t count = CONFIG_USB_EXTEND_SCREEN_FRAME_BUFFER_COUNT;
    s_usb.empty_frames = xQueueCreate(count, sizeof(frame_t *));
    s_usb.filled_frames = xQueueCreate(count, sizeof(frame_t *));
    s_usb.frames = heap_caps_calloc(count, sizeof(frame_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_usb.empty_frames != NULL && s_usb.filled_frames != NULL && s_usb.frames != NULL,
                        ESP_ERR_NO_MEM, TAG, "frame queues alloc failed");

    for (size_t i = 0; i < count; ++i) {
        frame_t *frame = &s_usb.frames[i];
        frame->capacity = FRAME_LIMIT_BYTES;
        frame->data = heap_caps_aligned_alloc(16, frame->capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(frame->data != NULL, ESP_ERR_NO_MEM, TAG, "frame buffer alloc failed");
        ESP_RETURN_ON_FALSE(xQueueSend(s_usb.empty_frames, &frame, 0) == pdPASS, ESP_FAIL, TAG, "queue frame failed");
    }
    return ESP_OK;
}

static esp_err_t init_usb_phy(void)
{
    usb_phy_config_t config = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_FULL,
    };
    return usb_new_phy(&config, &s_usb.phy);
}

static void tinyusb_task(void *arg)
{
    (void)arg;
    while (true) {
        tud_task();
    }
}

esp_err_t usb_extend_screen_init(subscreen_display_t *display)
{
    ESP_RETURN_ON_FALSE(display != NULL, ESP_ERR_INVALID_ARG, TAG, "display is null");
    if (s_usb.initialized) {
        return ESP_OK;
    }
    s_usb.display = display;
    s_usb.stats_window_started_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(allocate_frames(), TAG, "frame allocation failed");
    ESP_RETURN_ON_ERROR(init_usb_phy(), TAG, "USB PHY init failed");
    const tusb_rhport_init_t device_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL,
    };
    ESP_RETURN_ON_FALSE(tusb_init(0, &device_init), ESP_FAIL, TAG, "TinyUSB init failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(transfer_task, "usb_frame_draw", 6144, NULL,
                                    CONFIG_USB_EXTEND_SCREEN_TRANSFER_TASK_PRIORITY, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "transfer task create failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(tinyusb_task, "tinyusb", 4096, NULL, CONFIG_USB_TASK_PRIORITY, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "TinyUSB task create failed");
    s_usb.initialized = true;
    ESP_LOGI(TAG, "ready: mode=368x448 jpeg=%d fps=%d frame_limit=%uKiB buffers=%d",
             CONFIG_USB_EXTEND_SCREEN_JPEG_QUALITY, CONFIG_USB_EXTEND_SCREEN_MAX_FPS,
             CONFIG_USB_EXTEND_SCREEN_FRAME_LIMIT_KB, CONFIG_USB_EXTEND_SCREEN_FRAME_BUFFER_COUNT);
    return ESP_OK;
}

void tud_mount_cb(void)
{
    ESP_LOGI(TAG, "USB mounted");
    show_transport_status(SUBSCREEN_TRANSPORT_WAITING_FRAME);
}

void tud_umount_cb(void)
{
    ESP_LOGI(TAG, "USB unmounted");
    if (s_usb.display != NULL) {
        esp_err_t err = subscreen_display_show_placeholder(s_usb.display);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "restore placeholder failed: %s", esp_err_to_name(err));
        }
    }
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    ESP_LOGI(TAG, "USB suspended");
}

void tud_resume_cb(void)
{
    ESP_LOGI(TAG, "USB resumed");
}
