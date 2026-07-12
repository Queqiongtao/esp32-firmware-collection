#include "transport_usb.h"
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "frame_pipeline.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"

static const char *TAG = "transport_usb";
static usb_phy_handle_t s_phy;
static uint8_t s_header_bytes[sizeof(companion_frame_header_t)];
static size_t s_header_used;
static companion_frame_header_t s_header;
static uint8_t *s_payload;
static size_t s_payload_used;

static void reset_rx(void)
{
    s_header_used = 0;
    s_payload_used = 0;
    if (s_payload != NULL) { free(s_payload); s_payload = NULL; }
}

static void process_bytes(const uint8_t *data, size_t size)
{
    while (size > 0) {
        if (s_header_used < sizeof(s_header_bytes)) {
            const size_t take = (sizeof(s_header_bytes) - s_header_used < size) ? sizeof(s_header_bytes) - s_header_used : size;
            memcpy(s_header_bytes + s_header_used, data, take);
            s_header_used += take; data += take; size -= take;
            if (s_header_used != sizeof(s_header_bytes)) continue;
            memcpy(&s_header, s_header_bytes, sizeof(s_header));
            if (s_header.magic != COMPANION_FRAME_MAGIC || s_header.version != COMPANION_FRAME_VERSION ||
                s_header.header_size != sizeof(s_header) || s_header.payload_size == 0 ||
                s_header.payload_size > CONFIG_COMPANION_FRAME_LIMIT_KB * 1024U) {
                memmove(s_header_bytes, s_header_bytes + 1, sizeof(s_header_bytes) - 1);
                s_header_used = sizeof(s_header_bytes) - 1;
                continue;
            }
            s_payload = malloc(s_header.payload_size);
            if (s_payload == NULL) { reset_rx(); return; }
        }
        const size_t take = (s_header.payload_size - s_payload_used < size) ? s_header.payload_size - s_payload_used : size;
        memcpy(s_payload + s_payload_used, data, take);
        s_payload_used += take; data += take; size -= take;
        if (s_payload_used == s_header.payload_size) {
            frame_pipeline_submit_result_t result = frame_pipeline_submit(COMPANION_TRANSPORT_USB, &s_header, s_payload);
            if (result != FRAME_PIPELINE_SUBMIT_OK) {
                ESP_LOGW(TAG, "drop USB frame: %s",
                         esp_err_to_name(frame_pipeline_submit_result_to_esp_err(result)));
            }
            reset_rx();
        }
    }
}

void tud_vendor_rx_cb(uint8_t itf, const uint8_t *buffer, uint16_t size)
{
    (void)buffer; (void)size;
    uint8_t buffer_in[512];
    while (tud_vendor_n_available(itf) > 0) {
        uint32_t available = tud_vendor_n_available(itf);
        uint32_t read = tud_vendor_n_read(itf, buffer_in, available > sizeof(buffer_in) ? sizeof(buffer_in) : available);
        if (read == 0) break;
        process_bytes(buffer_in, read);
    }
}

static void usb_task(void *arg) { (void)arg; while (true) tud_task(); }

esp_err_t transport_usb_init(void)
{
    usb_phy_config_t phy = {.controller = USB_PHY_CTRL_OTG, .target = USB_PHY_TARGET_INT,
                            .otg_mode = USB_OTG_MODE_DEVICE, .otg_speed = USB_PHY_SPEED_FULL};
    ESP_RETURN_ON_ERROR(usb_new_phy(&phy, &s_phy), TAG, "USB PHY");
    const tusb_rhport_init_t init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
    ESP_RETURN_ON_FALSE(tusb_init(0, &init), ESP_FAIL, TAG, "TinyUSB init");
    ESP_RETURN_ON_FALSE(xTaskCreate(usb_task, "tinyusb", 4096, NULL, CONFIG_COMPANION_USB_TASK_PRIORITY, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "USB task");
    ESP_LOGI(TAG, "ready: Vendor Bulk VID=%04x PID=%04x", CONFIG_COMPANION_USB_VID, CONFIG_COMPANION_USB_PID);
    return ESP_OK;
}

void tud_mount_cb(void) { ESP_LOGI(TAG, "USB mounted"); }
void tud_umount_cb(void) { reset_rx(); frame_pipeline_release(COMPANION_TRANSPORT_USB); ESP_LOGI(TAG, "USB unmounted"); }
