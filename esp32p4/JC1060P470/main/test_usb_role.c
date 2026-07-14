/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "test_usb_role.h"

#include <inttypes.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "test_config.h"
#include "test_usb.h"
#include "test_uvc.h"

static const char *TAG = "test_usb_role";

#define TEST_USB_ROLE_QUEUE_LEN         4
#define TEST_USB_ROLE_TASK_STACK_SIZE   8192
#define TEST_USB_ROLE_TASK_PRIORITY     3
#define TEST_USB_ROLE_TASK_CORE_ID      tskNO_AFFINITY

static QueueHandle_t s_queue;
static TaskHandle_t s_task_handle;
static test_usb_role_t s_current_role = TEST_USB_ROLE_NONE;
static bool s_switch_in_progress;
static esp_err_t s_last_switch_error = ESP_OK;
static bool s_initialized;

static const char *uvc_role_name(test_usb_role_t role)
{
    switch (role) {
    case TEST_USB_ROLE_CDC_DEVICE: return "CDC Device";
    case TEST_USB_ROLE_UVC_HOST:   return "UVC Host";
    default:                       return "None";
    }
}

/* Execute the role switch synchronously in the worker task context. */
static esp_err_t uvc_role_do_switch(test_usb_role_t target)
{
    test_usb_role_t current = s_current_role;
    esp_err_t ret = ESP_OK;

    if (current == target) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Switching USB role: %s -> %s", uvc_role_name(current), uvc_role_name(target));

    /* Phase 1: deinit current role */
    if (current == TEST_USB_ROLE_CDC_DEVICE) {
        test_usb_deinit();
    } else if (current == TEST_USB_ROLE_UVC_HOST) {
        test_uvc_deinit();
    }
    s_current_role = TEST_USB_ROLE_NONE;

    /* Phase 2: init target role */
    if (target == TEST_USB_ROLE_CDC_DEVICE) {
        ret = test_usb_init();
        if (ret == ESP_OK) {
            ret = test_usb_run();
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "CDC Device init failed: %s", esp_err_to_name(ret));
            test_usb_deinit();
        }
    } else if (target == TEST_USB_ROLE_UVC_HOST) {
        ret = test_uvc_init();
        if (ret == ESP_OK) {
            /* Try to open/start the stream. If no camera is connected yet,
             * uvc_host_stream_open will time out; the driver remains installed
             * and the stream can be reopened later. */
            esp_err_t run_ret = test_uvc_run();
            if (run_ret != ESP_OK) {
                ESP_LOGW(TAG, "UVC stream open/start failed (device may not be connected): %s",
                         esp_err_to_name(run_ret));
            }
        } else {
            ESP_LOGE(TAG, "UVC Host init failed: %s", esp_err_to_name(ret));
            test_uvc_deinit();
        }
    }

    if (ret == ESP_OK) {
        s_current_role = target;
        ESP_LOGI(TAG, "USB role switched to %s", uvc_role_name(target));
    } else {
        s_current_role = TEST_USB_ROLE_NONE;
        ESP_LOGE(TAG, "USB role switch to %s failed, now idle", uvc_role_name(target));
    }
    s_last_switch_error = ret;
    return ret;
}

static void uvc_role_worker_task(void *arg)
{
    (void)arg;

    test_usb_role_t msg;
    while (true) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_switch_in_progress = true;
        uvc_role_do_switch(msg);
        s_switch_in_progress = false;
    }

    vTaskDelete(NULL);
}

esp_err_t test_usb_role_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing USB role management framework");

    s_current_role = TEST_USB_ROLE_NONE;
    s_switch_in_progress = false;
    s_last_switch_error = ESP_OK;

    s_queue = xQueueCreate(TEST_USB_ROLE_QUEUE_LEN, sizeof(test_usb_role_t));
    ESP_RETURN_ON_FALSE(s_queue != NULL, ESP_ERR_NO_MEM, TAG, "create role queue failed");

    BaseType_t task_created = xTaskCreatePinnedToCore(uvc_role_worker_task,
                                                      "usb_role",
                                                      TEST_USB_ROLE_TASK_STACK_SIZE,
                                                      NULL,
                                                      TEST_USB_ROLE_TASK_PRIORITY,
                                                      &s_task_handle,
                                                      TEST_USB_ROLE_TASK_CORE_ID);
    ESP_RETURN_ON_FALSE(task_created == pdTRUE, ESP_ERR_NO_MEM, TAG, "create role worker task failed");

    s_initialized = true;

    /* Queue the startup role based on Kconfig */
#if CONFIG_EXAMPLE_ENABLE_TEST_USB
    ESP_LOGI(TAG, "Startup role: CDC Device");
    test_usb_role_switch(TEST_USB_ROLE_CDC_DEVICE);
#elif CONFIG_EXAMPLE_ENABLE_TEST_UVC
    ESP_LOGI(TAG, "Startup role: UVC Host");
    test_usb_role_switch(TEST_USB_ROLE_UVC_HOST);
#else
    ESP_LOGI(TAG, "Startup role: None (no USB module enabled in Kconfig)");
#endif

    return ESP_OK;
}

esp_err_t test_usb_role_switch(test_usb_role_t target)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "role module not initialized");
    ESP_RETURN_ON_FALSE(target <= TEST_USB_ROLE_UVC_HOST, ESP_ERR_INVALID_ARG, TAG, "invalid target role");

    if (xQueueSend(s_queue, &target, 0) != pdTRUE) {
        ESP_LOGW(TAG, "role switch queue full, dropping request");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

test_usb_role_t test_usb_role_current(void)
{
    return s_current_role;
}

esp_err_t test_usb_role_get_status(test_usb_role_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != NULL, ESP_ERR_INVALID_ARG, TAG, "out_status is NULL");
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "role module not initialized");

    test_usb_role_status_t status = {
        .current_role = s_current_role,
        .target_role = s_current_role,
        .switch_in_progress = s_switch_in_progress,
        .last_switch_error = s_last_switch_error,
    };

    /* Mirror CDC Device status */
    test_usb_status_t cdc_status = {0};
    if (test_usb_get_status(&cdc_status) == ESP_OK) {
        status.cdc_attached = cdc_status.attached;
        status.cdc_rx_packets = cdc_status.rx_packets;
        status.cdc_tx_packets = cdc_status.tx_packets;
    }

    /* Mirror UVC Host status */
    test_uvc_status_t uvc_status = {0};
    if (test_uvc_get_status(&uvc_status) == ESP_OK) {
        status.uvc_device_connected = uvc_status.device_connected;
        status.uvc_stream_started = uvc_status.stream_started;
        status.uvc_format_negotiated = uvc_status.format_negotiated;
        status.uvc_negotiated_h_res = uvc_status.negotiated_h_res;
        status.uvc_negotiated_v_res = uvc_status.negotiated_v_res;
        status.uvc_negotiated_fps = uvc_status.negotiated_fps;
        status.uvc_frames_decoded = uvc_status.frames_decoded;
        status.uvc_decode_errors = uvc_status.decode_errors;
    }

    *out_status = status;
    return ESP_OK;
}