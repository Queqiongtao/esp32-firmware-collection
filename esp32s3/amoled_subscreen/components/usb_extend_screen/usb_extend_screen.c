#include "usb_extend_screen.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tusb.h"
#include "usb_descriptors.h"

static const char *TAG = "usb_extend_screen";

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
} frame_t;

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
    uint32_t received_packets;
    uint32_t received_bytes;
    uint32_t accepted_frames;
    uint32_t completed_frames;
    uint32_t rejected_frames;
    bool initialized;
} s_usb;

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
        header->payload_total == 0 || header->payload_total > CONFIG_USB_EXTEND_SCREEN_FRAME_LIMIT_B) {
        s_usb.rejected_frames++;
        s_usb.skip_remaining = header->payload_total;
        ESP_LOGW(TAG, "drop header: type=%u area=%u,%u %ux%u payload=%" PRIu32,
                 header->type, header->x, header->y, header->width, header->height,
                 (uint32_t)header->payload_total);
        show_transport_status(SUBSCREEN_TRANSPORT_FRAME_REJECTED);
        return;
    }

    frame_t *frame = NULL;
    if (xQueueReceive(s_usb.empty_frames, &frame, 0) != pdPASS) {
        s_usb.rejected_frames++;
        s_usb.skip_remaining = header->payload_total;
        ESP_LOGW(TAG, "drop frame %u: no free buffer", header->frame_id);
        show_transport_status(SUBSCREEN_TRANSPORT_FRAME_REJECTED);
        return;
    }
    frame->received = 0;
    frame->total = header->payload_total;
    frame->width = header->width;
    frame->height = header->height;
    s_usb.current_frame = frame;
    s_usb.accepted_frames++;
    show_transport_status(SUBSCREEN_TRANSPORT_WAITING_FRAME);
}

static void consume_vendor_bytes(const uint8_t *data, size_t size)
{
    while (size > 0) {
        if (s_usb.skip_remaining > 0) {
            const size_t consumed = size < s_usb.skip_remaining ? size : s_usb.skip_remaining;
            data += consumed;
            size -= consumed;
            s_usb.skip_remaining -= consumed;
            continue;
        }

        if (s_usb.current_frame != NULL) {
            frame_t *frame = s_usb.current_frame;
            const size_t remaining = frame->total - frame->received;
            const size_t consumed = size < remaining ? size : remaining;
            memcpy(frame->data + frame->received, data, consumed);
            frame->received += consumed;
            data += consumed;
            size -= consumed;
            if (frame->received == frame->total) {
                if (xQueueSend(s_usb.filled_frames, &frame, 0) != pdPASS) {
                    ESP_LOGW(TAG, "filled frame queue full");
                    return_current_frame_to_empty();
                    s_usb.rejected_frames++;
                    show_transport_status(SUBSCREEN_TRANSPORT_FRAME_REJECTED);
                } else {
                    s_usb.completed_frames++;
                    s_usb.current_frame = NULL;
                }
            }
            continue;
        }

        const size_t header_remaining = sizeof(s_usb.header_data) - s_usb.header_received;
        const size_t consumed = size < header_remaining ? size : header_remaining;
        memcpy(s_usb.header_data + s_usb.header_received, data, consumed);
        s_usb.header_received += consumed;
        data += consumed;
        size -= consumed;
        if (s_usb.header_received == sizeof(s_usb.header_data)) {
            begin_frame_from_header();
        }
    }
}

void tud_vendor_rx_cb(uint8_t itf, const uint8_t *buffer, uint16_t size)
{
    (void)buffer;
    (void)size;
    s_usb.received_packets++;

    uint8_t rx_buffer[CFG_TUD_VENDOR_RX_BUFSIZE];
    while (tud_vendor_n_available(itf) > 0) {
        const uint32_t available = tud_vendor_n_available(itf);
        const uint32_t read_len = available < sizeof(rx_buffer) ? available : sizeof(rx_buffer);
        const uint32_t read = tud_vendor_n_read(itf, rx_buffer, read_len);
        if (read == 0) {
            break;
        }
        s_usb.received_bytes += read;
        show_transport_status(SUBSCREEN_TRANSPORT_RECEIVING_FRAME);
        consume_vendor_bytes(rx_buffer, read);
    }
}

static void transfer_task(void *arg)
{
    (void)arg;
    while (true) {
        frame_t *frame = NULL;
        if (xQueueReceive(s_usb.filled_frames, &frame, portMAX_DELAY) != pdPASS) {
            continue;
        }
        esp_err_t err = subscreen_display_draw_jpeg(s_usb.display, frame->data, frame->received,
                                                    frame->width, frame->height);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "frame draw failed: %s", esp_err_to_name(err));
            show_transport_status(SUBSCREEN_TRANSPORT_FRAME_FAILED);
        }
        return_empty(frame);
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
        frame->capacity = CONFIG_USB_EXTEND_SCREEN_FRAME_LIMIT_B;
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
    ESP_LOGI(TAG, "ready: modes=368x448,448x368 jpeg=%d fps=%d", CONFIG_USB_EXTEND_SCREEN_JPEG_QUALITY,
             CONFIG_USB_EXTEND_SCREEN_MAX_FPS);
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
