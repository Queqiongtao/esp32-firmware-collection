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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "test_config.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"

static const char *TAG = "test_usb";

typedef enum {
    USB_APP_EVENT_RX = 0,
    USB_APP_EVENT_STOP,
} usb_app_event_t;

typedef struct {
    tinyusb_cdcacm_itf_t cdc_port;
    usb_app_event_t event;
} usb_app_queue_msg_t;

static QueueHandle_t s_event_queue;
static EventGroupHandle_t s_lifecycle_events;
static TaskHandle_t s_worker_task_handle;
static bool s_initialized;
static volatile bool s_stopping;
static bool s_cdc_installed;
static bool s_driver_installed;
static bool s_controller_released = true;
static bool s_attached;
static bool s_dtr;
static bool s_rts;
static uint32_t s_rx_packets;
static uint32_t s_tx_packets;
static uint32_t s_rx_bytes;
static uint32_t s_tx_bytes;
static size_t s_last_rx_size;
static size_t s_last_tx_size;
static uint32_t s_rx_events_dropped;
static esp_err_t s_last_error = ESP_OK;
static test_module_meta_t s_module = {
    .state = TEST_MODULE_STATE_UNINITIALIZED,
    .last_error = ESP_OK,
};
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;

#define USB_WORKER_EXITED_BIT BIT0
#define USB_WORKER_STOP_TIMEOUT_MS 1000

static void usb_set_module_state(test_module_state_t state, esp_err_t error, bool new_generation)
{
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_status_lock);
    if (new_generation) {
        s_module.generation++;
    }
    s_module.state = state;
    s_module.last_error = error;
    s_module.updated_at_us = now_us;
    s_last_error = error;
    portEXIT_CRITICAL(&s_status_lock);
}

static bool usb_worker_running(void)
{
    portENTER_CRITICAL(&s_status_lock);
    bool running = s_worker_task_handle != NULL;
    portEXIT_CRITICAL(&s_status_lock);
    return running;
}

static esp_err_t usb_wait_worker_stopped(void)
{
    const int64_t deadline_us = esp_timer_get_time() + USB_WORKER_STOP_TIMEOUT_MS * 1000LL;
    while (usb_worker_running()) {
        if (esp_timer_get_time() >= deadline_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return ESP_OK;
}

static void usb_device_event_callback(tinyusb_event_t *event, void *arg)
{
    (void)arg;

    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        portENTER_CRITICAL(&s_status_lock);
        s_attached = true;
        portEXIT_CRITICAL(&s_status_lock);
        ESP_LOGI(TAG, "USB device attached on rhport %" PRIu8, event->rhport);
        break;
    case TINYUSB_EVENT_DETACHED:
        portENTER_CRITICAL(&s_status_lock);
        s_attached = false;
        portEXIT_CRITICAL(&s_status_lock);
        ESP_LOGI(TAG, "USB device detached on rhport %" PRIu8, event->rhport);
        break;
    default:
        ESP_LOGI(TAG, "USB device event %d on rhport %" PRIu8, event->id, event->rhport);
        break;
    }
}

static void usb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event)
{
    portENTER_CRITICAL(&s_status_lock);
    s_dtr = event->line_state_changed_data.dtr;
    s_rts = event->line_state_changed_data.rts;
    portEXIT_CRITICAL(&s_status_lock);
    ESP_LOGI(TAG, "CDC port %d line state changed: DTR=%d RTS=%d",
             itf,
             event->line_state_changed_data.dtr,
             event->line_state_changed_data.rts);
}

static void usb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    (void)event;

    portENTER_CRITICAL(&s_status_lock);
    const bool stopping = s_stopping;
    portEXIT_CRITICAL(&s_status_lock);
    if (s_event_queue == NULL || stopping) {
        return;
    }

    const usb_app_queue_msg_t msg = {
        .cdc_port = (tinyusb_cdcacm_itf_t)itf,
        .event = USB_APP_EVENT_RX,
    };

    if (xQueueSend(s_event_queue, &msg, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_status_lock);
        uint32_t dropped = ++s_rx_events_dropped;
        portEXIT_CRITICAL(&s_status_lock);
        if (dropped <= 3 || (dropped & (dropped - 1U)) == 0) {
            ESP_LOGW(TAG, "USB event queue full, dropped RX events=%" PRIu32, dropped);
        }
    }
}

static void usb_echo_rx_data(tinyusb_cdcacm_itf_t cdc_port)
{
    uint8_t rx_buf[TEST_USB_CDC_RX_BUF_SIZE];
    size_t rx_size = 0;

    do {
        esp_err_t ret = tinyusb_cdcacm_read(cdc_port, rx_buf, sizeof(rx_buf), &rx_size);
        if (ret != ESP_OK) {
            usb_set_module_state(TEST_MODULE_STATE_RUNNING, ret, false);
            ESP_LOGE(TAG, "CDC read failed on port %d: %s", cdc_port, esp_err_to_name(ret));
            return;
        }

        if (rx_size == 0) {
            return;
        }

        ESP_LOGD(TAG, "CDC RX %u bytes on port %d", (unsigned int)rx_size, cdc_port);
        ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buf, rx_size, ESP_LOG_DEBUG);
        portENTER_CRITICAL(&s_status_lock);
        s_rx_packets++;
        s_rx_bytes += (uint32_t)rx_size;
        s_last_rx_size = rx_size;
        portEXIT_CRITICAL(&s_status_lock);

        size_t queued = tinyusb_cdcacm_write_queue(cdc_port, rx_buf, rx_size);
        if (queued != rx_size) {
            ESP_LOGW(TAG, "CDC TX queue accepted %u/%u bytes on port %d",
                     (unsigned int)queued, (unsigned int)rx_size, cdc_port);
        }

        ret = tinyusb_cdcacm_write_flush(cdc_port, TEST_USB_CDC_TX_FLUSH_TIMEOUT_TICKS);
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED) {
            usb_set_module_state(TEST_MODULE_STATE_RUNNING, ret, false);
            ESP_LOGE(TAG, "CDC write flush failed on port %d: %s", cdc_port, esp_err_to_name(ret));
            return;
        }
        portENTER_CRITICAL(&s_status_lock);
        s_tx_packets++;
        s_tx_bytes += (uint32_t)queued;
        s_last_tx_size = queued;
        s_last_error = ESP_OK;
        s_module.last_error = ESP_OK;
        portEXIT_CRITICAL(&s_status_lock);
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

        if (msg.event == USB_APP_EVENT_STOP) {
            break;
        } else if (msg.event == USB_APP_EVENT_RX) {
            usb_echo_rx_data(msg.cdc_port);
        }
    }

    if (s_lifecycle_events != NULL) {
        xEventGroupSetBits(s_lifecycle_events, USB_WORKER_EXITED_BIT);
    }
    portENTER_CRITICAL(&s_status_lock);
    s_worker_task_handle = NULL;
    portEXIT_CRITICAL(&s_status_lock);
    vTaskDelete(NULL);
}

static void usb_cleanup_app_resources(void)
{
    if (s_event_queue != NULL) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
    }
    if (s_lifecycle_events != NULL) {
        vEventGroupDelete(s_lifecycle_events);
        s_lifecycle_events = NULL;
    }
}

static esp_err_t usb_stop_worker(void)
{
    if (!usb_worker_running()) {
        return ESP_OK;
    }

    s_stopping = true;
    const usb_app_queue_msg_t stop = {
        .cdc_port = TEST_USB_CDC_PORT,
        .event = USB_APP_EVENT_STOP,
    };
    (void)xQueueSendToFront(s_event_queue, &stop, 0);
    EventBits_t bits = xEventGroupWaitBits(s_lifecycle_events, USB_WORKER_EXITED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(USB_WORKER_STOP_TIMEOUT_MS));
    if ((bits & USB_WORKER_EXITED_BIT) == 0) {
        return ESP_ERR_TIMEOUT;
    }
    return usb_wait_worker_stopped();
}

esp_err_t test_usb_init(void)
{
    esp_err_t ret = ESP_OK;

    if (s_initialized && s_module.state != TEST_MODULE_STATE_ERROR) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(!s_driver_installed && s_worker_task_handle == NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "previous USB teardown incomplete");

    ESP_LOGI(TAG, "Initializing USB Device CDC-ACM echo test module");
    usb_set_module_state(TEST_MODULE_STATE_INITIALIZING, ESP_OK, false);
    portENTER_CRITICAL(&s_status_lock);
    s_initialized = true;
    s_stopping = false;
    s_controller_released = true;
    portEXIT_CRITICAL(&s_status_lock);

    s_lifecycle_events = xEventGroupCreate();
    ESP_GOTO_ON_FALSE(s_lifecycle_events != NULL, ESP_ERR_NO_MEM, fail,
                      TAG, "create USB lifecycle event group failed");

    s_event_queue = xQueueCreate(TEST_USB_CDC_EVENT_QUEUE_LEN, sizeof(usb_app_queue_msg_t));
    ESP_GOTO_ON_FALSE(s_event_queue != NULL, ESP_ERR_NO_MEM, fail,
                      TAG, "create USB event queue failed");

    BaseType_t task_created = xTaskCreatePinnedToCore(usb_event_task,
                                                      "usb_cdc_evt",
                                                      TEST_USB_CDC_TASK_STACK_SIZE,
                                                      NULL,
                                                      TEST_USB_CDC_TASK_PRIORITY,
                                                      &s_worker_task_handle,
                                                      TEST_USB_CDC_TASK_CORE_ID);
    ESP_GOTO_ON_FALSE(task_created == pdTRUE, ESP_ERR_NO_MEM, fail, TAG, "create USB worker task failed");

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(usb_device_event_callback, NULL);
    ESP_GOTO_ON_ERROR(tinyusb_driver_install(&tusb_cfg), fail, TAG, "tinyusb_driver_install failed");
    s_driver_installed = true;
    portENTER_CRITICAL(&s_status_lock);
    s_controller_released = false;
    portEXIT_CRITICAL(&s_status_lock);

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TEST_USB_CDC_PORT,
        .callback_rx = usb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    ESP_GOTO_ON_ERROR(tinyusb_cdcacm_init(&acm_cfg), fail, TAG, "tinyusb_cdcacm_init failed");
    s_cdc_installed = true;
    ESP_GOTO_ON_ERROR(tinyusb_cdcacm_register_callback(TEST_USB_CDC_PORT,
                                                       CDC_EVENT_LINE_STATE_CHANGED,
                                                       usb_cdc_line_state_changed_callback),
                      fail, TAG, "register line state callback failed");

    portENTER_CRITICAL(&s_status_lock);
    s_attached = false;
    s_dtr = false;
    s_rts = false;
    s_rx_packets = 0;
    s_tx_packets = 0;
    s_rx_bytes = 0;
    s_tx_bytes = 0;
    s_last_rx_size = 0;
    s_last_tx_size = 0;
    s_rx_events_dropped = 0;
    s_last_error = ESP_OK;
    portEXIT_CRITICAL(&s_status_lock);
    usb_set_module_state(TEST_MODULE_STATE_RUNNING, ESP_OK, true);
    ESP_LOGI(TAG, "USB CDC-ACM initialized on High-Speed USB Device port, waiting for host enumeration");
    return ESP_OK;

fail: {
    const esp_err_t init_error = ret;
    esp_err_t cleanup_error = test_usb_deinit();
    if (cleanup_error != ESP_OK) {
        ESP_LOGE(TAG, "USB init rollback incomplete: %s", esp_err_to_name(cleanup_error));
    }
    usb_set_module_state(TEST_MODULE_STATE_ERROR, init_error, false);
    return ret;
}
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

    portENTER_CRITICAL(&s_status_lock);
    *out_status = (test_usb_status_t) {
        .module = s_module,
        .initialized = s_initialized,
        .controller_released = s_controller_released,
        .attached = s_attached,
        .dtr = s_dtr,
        .rts = s_rts,
        .rx_packets = s_rx_packets,
        .tx_packets = s_tx_packets,
        .rx_bytes = s_rx_bytes,
        .tx_bytes = s_tx_bytes,
        .last_rx_size = s_last_rx_size,
        .last_tx_size = s_last_tx_size,
        .rx_events_dropped = s_rx_events_dropped,
        .last_error = s_last_error,
    };
    portEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}

esp_err_t test_usb_deinit(void)
{
    if (!s_initialized && !s_driver_installed && !s_cdc_installed &&
        s_worker_task_handle == NULL && s_event_queue == NULL &&
        s_lifecycle_events == NULL) {
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_status_lock);
    s_stopping = true;
    portEXIT_CRITICAL(&s_status_lock);
    usb_set_module_state(TEST_MODULE_STATE_STOPPING, ESP_OK, false);
    esp_err_t first_error = usb_stop_worker();
    if (first_error != ESP_OK) {
        ESP_LOGE(TAG, "USB worker did not acknowledge stop; resources retained");
        usb_set_module_state(TEST_MODULE_STATE_ERROR, first_error, false);
        return first_error;
    }

    if (s_cdc_installed) {
        esp_err_t ret = tinyusb_cdcacm_deinit(TEST_USB_CDC_PORT);
        if (ret == ESP_OK) {
            s_cdc_installed = false;
        } else {
            first_error = ret;
            ESP_LOGW(TAG, "tinyusb_cdcacm_deinit failed: %s", esp_err_to_name(ret));
        }
    }

    if (s_driver_installed) {
        esp_err_t ret = tinyusb_driver_uninstall();
        if (ret != ESP_OK) {
            if (first_error == ESP_OK) {
                first_error = ret;
            }
            portENTER_CRITICAL(&s_status_lock);
            s_controller_released = false;
            portEXIT_CRITICAL(&s_status_lock);
            ESP_LOGE(TAG, "tinyusb_driver_uninstall failed; callback resources retained: %s",
                     esp_err_to_name(ret));
            usb_set_module_state(TEST_MODULE_STATE_ERROR, ret, false);
            return first_error;
        }
        s_driver_installed = false;
        s_cdc_installed = false;
        portENTER_CRITICAL(&s_status_lock);
        s_controller_released = true;
        portEXIT_CRITICAL(&s_status_lock);
    }

    usb_cleanup_app_resources();
    portENTER_CRITICAL(&s_status_lock);
    s_initialized = false;
    s_stopping = false;
    s_attached = false;
    s_dtr = false;
    s_rts = false;
    portEXIT_CRITICAL(&s_status_lock);
    usb_set_module_state(first_error == ESP_OK ? TEST_MODULE_STATE_UNINITIALIZED
                                               : TEST_MODULE_STATE_ERROR,
                         first_error, false);
    ESP_LOGI(TAG, "USB Device CDC-ACM deinitialized: %s", esp_err_to_name(first_error));
    return first_error;
}
