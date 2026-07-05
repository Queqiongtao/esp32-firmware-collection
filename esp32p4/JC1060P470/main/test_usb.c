/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "test_usb.h"

#include <inttypes.h>
#include <string.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "test_config.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"

static const char *TAG = "test_usb";

typedef enum {
    USB_APP_EVENT_RX = 0,
} usb_app_event_t;

typedef struct {
    tinyusb_cdcacm_itf_t cdc_port;
    usb_app_event_t event;
} usb_app_queue_msg_t;

static QueueHandle_t s_event_queue;
static TaskHandle_t s_worker_task_handle;
static bool s_initialized;
static bool s_stopping;
static bool s_attached;
static bool s_dtr;
static bool s_rts;
static uint32_t s_rx_packets;
static uint32_t s_tx_packets;
static uint32_t s_rx_bytes;
static uint32_t s_tx_bytes;
static size_t s_last_rx_size;
static size_t s_last_tx_size;
static esp_err_t s_last_error = ESP_OK;

static void usb_device_event_callback(tinyusb_event_t *event, void *arg)
{
    (void)arg;

    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        s_attached = true;
        ESP_LOGI(TAG, "USB device attached on rhport %" PRIu8, event->rhport);
        break;
    case TINYUSB_EVENT_DETACHED:
        s_attached = false;
        ESP_LOGI(TAG, "USB device detached on rhport %" PRIu8, event->rhport);
        break;
    default:
        ESP_LOGI(TAG, "USB device event %d on rhport %" PRIu8, event->id, event->rhport);
        break;
    }
}

static void usb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event)
{
    s_dtr = event->line_state_changed_data.dtr;
    s_rts = event->line_state_changed_data.rts;
    ESP_LOGI(TAG, "CDC port %d line state changed: DTR=%d RTS=%d",
             itf,
             event->line_state_changed_data.dtr,
             event->line_state_changed_data.rts);
}

static void usb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    (void)event;

    if (s_event_queue == NULL || s_stopping) {
        return;
    }

    const usb_app_queue_msg_t msg = {
        .cdc_port = (tinyusb_cdcacm_itf_t)itf,
        .event = USB_APP_EVENT_RX,
    };

    if (xQueueSend(s_event_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "USB event queue full, dropping RX event");
    }
}

static void usb_echo_rx_data(tinyusb_cdcacm_itf_t cdc_port)
{
    uint8_t rx_buf[TEST_USB_CDC_RX_BUF_SIZE];
    size_t rx_size = 0;

    do {
        esp_err_t ret = tinyusb_cdcacm_read(cdc_port, rx_buf, sizeof(rx_buf), &rx_size);
        if (ret != ESP_OK) {
            s_last_error = ret;
            ESP_LOGE(TAG, "CDC read failed on port %d: %s", cdc_port, esp_err_to_name(ret));
            return;
        }

        if (rx_size == 0) {
            return;
        }

        ESP_LOGI(TAG, "CDC RX %u bytes on port %d", (unsigned int)rx_size, cdc_port);
        ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buf, rx_size, ESP_LOG_INFO);
        s_rx_packets++;
        s_rx_bytes += (uint32_t)rx_size;
        s_last_rx_size = rx_size;

        size_t queued = tinyusb_cdcacm_write_queue(cdc_port, rx_buf, rx_size);
        if (queued != rx_size) {
            ESP_LOGW(TAG, "CDC TX queue accepted %u/%u bytes on port %d",
                     (unsigned int)queued, (unsigned int)rx_size, cdc_port);
        }

        ret = tinyusb_cdcacm_write_flush(cdc_port, TEST_USB_CDC_TX_FLUSH_TIMEOUT_TICKS);
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED) {
            s_last_error = ret;
            ESP_LOGE(TAG, "CDC write flush failed on port %d: %s", cdc_port, esp_err_to_name(ret));
            return;
        }
        s_tx_packets++;
        s_tx_bytes += (uint32_t)queued;
        s_last_tx_size = queued;
        s_last_error = ESP_OK;
    } while (rx_size == sizeof(rx_buf));
}

static void usb_event_task(void *arg)
{
    (void)arg;

    usb_app_queue_msg_t msg;
    while (!s_stopping) {
        if (xQueueReceive(s_event_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        if (msg.event == USB_APP_EVENT_RX) {
            usb_echo_rx_data(msg.cdc_port);
        }
    }

    s_worker_task_handle = NULL;
    vTaskDelete(NULL);
}

static void usb_cleanup_app_resources(void)
{
    if (s_worker_task_handle != NULL) {
        vTaskDelete(s_worker_task_handle);
        s_worker_task_handle = NULL;
    }

    if (s_event_queue != NULL) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
    }
}

esp_err_t test_usb_init(void)
{
    esp_err_t ret = ESP_OK;

    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing USB Device CDC-ACM echo test module");
    s_stopping = false;

    s_event_queue = xQueueCreate(TEST_USB_CDC_EVENT_QUEUE_LEN, sizeof(usb_app_queue_msg_t));
    ESP_RETURN_ON_FALSE(s_event_queue != NULL, ESP_ERR_NO_MEM, TAG, "create USB event queue failed");

    BaseType_t task_created = xTaskCreatePinnedToCore(usb_event_task,
                                                      "usb_cdc_evt",
                                                      TEST_USB_CDC_TASK_STACK_SIZE,
                                                      NULL,
                                                      TEST_USB_CDC_TASK_PRIORITY,
                                                      &s_worker_task_handle,
                                                      TEST_USB_CDC_TASK_CORE_ID);
    ESP_GOTO_ON_FALSE(task_created == pdTRUE, ESP_ERR_NO_MEM, err, TAG, "create USB worker task failed");

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(usb_device_event_callback, NULL);
    ESP_GOTO_ON_ERROR(tinyusb_driver_install(&tusb_cfg), err, TAG, "tinyusb_driver_install failed");

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TEST_USB_CDC_PORT,
        .callback_rx = usb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    ESP_GOTO_ON_ERROR(tinyusb_cdcacm_init(&acm_cfg), err_uninstall, TAG, "tinyusb_cdcacm_init failed");
    ESP_GOTO_ON_ERROR(tinyusb_cdcacm_register_callback(TEST_USB_CDC_PORT,
                                                       CDC_EVENT_LINE_STATE_CHANGED,
                                                       usb_cdc_line_state_changed_callback),
                      err_deinit_cdc, TAG, "register line state callback failed");

    s_initialized = true;
    s_attached = false;
    s_dtr = false;
    s_rts = false;
    s_rx_packets = 0;
    s_tx_packets = 0;
    s_rx_bytes = 0;
    s_tx_bytes = 0;
    s_last_rx_size = 0;
    s_last_tx_size = 0;
    s_last_error = ESP_OK;
    ESP_LOGI(TAG, "USB CDC-ACM initialized on High-Speed USB Device port, waiting for host enumeration");
    return ESP_OK;

err_deinit_cdc:
    tinyusb_cdcacm_deinit(TEST_USB_CDC_PORT);
err_uninstall:
    tinyusb_driver_uninstall();
err:
    usb_cleanup_app_resources();
    return ret;
}

esp_err_t test_usb_run(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "USB not initialized, call test_usb_init first");
    return ESP_OK;
}

esp_err_t test_usb_get_status(test_usb_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != NULL, ESP_ERR_INVALID_ARG, TAG, "out_status is NULL");

    *out_status = (test_usb_status_t) {
        .initialized = s_initialized,
        .attached = s_attached,
        .dtr = s_dtr,
        .rts = s_rts,
        .rx_packets = s_rx_packets,
        .tx_packets = s_tx_packets,
        .rx_bytes = s_rx_bytes,
        .tx_bytes = s_tx_bytes,
        .last_rx_size = s_last_rx_size,
        .last_tx_size = s_last_tx_size,
        .last_error = s_last_error,
    };
    return ESP_OK;
}

esp_err_t test_usb_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    s_stopping = true;
    if (s_worker_task_handle != NULL) {
        for (int i = 0; i < 10 && s_worker_task_handle != NULL; ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_worker_task_handle != NULL) {
            ESP_LOGW(TAG, "USB worker task did not exit in time, deleting it");
            vTaskDelete(s_worker_task_handle);
            s_worker_task_handle = NULL;
        }
    }

    esp_err_t ret = tinyusb_cdcacm_deinit(TEST_USB_CDC_PORT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "tinyusb_cdcacm_deinit failed: %s", esp_err_to_name(ret));
    }

    ret = tinyusb_driver_uninstall();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "tinyusb_driver_uninstall failed: %s", esp_err_to_name(ret));
    }

    usb_cleanup_app_resources();
    s_initialized = false;
    s_stopping = false;
    s_attached = false;
    s_dtr = false;
    s_rts = false;
    s_rx_packets = 0;
    s_tx_packets = 0;
    s_rx_bytes = 0;
    s_tx_bytes = 0;
    s_last_rx_size = 0;
    s_last_tx_size = 0;
    s_last_error = ESP_OK;
    ESP_LOGI(TAG, "USB Device CDC-ACM deinitialized");
    return ESP_OK;
}
