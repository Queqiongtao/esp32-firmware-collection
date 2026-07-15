/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "test_usb_role.h"

#include <inttypes.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
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
#define TEST_USB_ROLE_TASK_CORE_ID      1

static QueueHandle_t s_queue;
static TaskHandle_t s_task_handle;
static test_usb_role_t s_current_role = TEST_USB_ROLE_NONE;
static test_usb_role_t s_target_role = TEST_USB_ROLE_NONE;
static bool s_switch_in_progress;
static esp_err_t s_last_switch_error = ESP_OK;
static bool s_initialized;
static test_uvc_recovery_level_t s_auto_recovery_level = TEST_UVC_RECOVERY_NONE;
static uint32_t s_auto_recovery_attempt;
static int64_t s_next_recovery_us;
static bool s_auto_recovery_exhausted;

typedef struct {
    test_usb_role_t target;
    bool force_restart;
    uint32_t uvc_h_res;
    uint32_t uvc_v_res;
} test_usb_role_command_t;

static const char *uvc_role_name(test_usb_role_t role)
{
    switch (role) {
    case TEST_USB_ROLE_CDC_DEVICE: return "CDC Device";
    case TEST_USB_ROLE_UVC_HOST:   return "UVC Host";
    default:                       return "None";
    }
}

static void uvc_role_reset_recovery(void)
{
    s_auto_recovery_level = TEST_UVC_RECOVERY_NONE;
    s_auto_recovery_attempt = 0;
    s_next_recovery_us = 0;
    s_auto_recovery_exhausted = false;
}

/* Execute the role switch synchronously in the worker task context. */
static esp_err_t uvc_role_do_switch(const test_usb_role_command_t *command)
{
    const test_usb_role_t target = command->target;
    const bool force_restart = command->force_restart;
    test_usb_role_t current = s_current_role;
    esp_err_t ret = ESP_OK;

    if (current == target && !force_restart) {
        return ESP_OK;
    }

    if (current == TEST_USB_ROLE_UVC_HOST && target == TEST_USB_ROLE_UVC_HOST && force_restart) {
        ESP_LOGI(TAG, "Changing UVC stream resolution to %ux%u", (unsigned)command->uvc_h_res,
                 (unsigned)command->uvc_v_res);
        ret = test_uvc_change_resolution(command->uvc_h_res, command->uvc_v_res);
        s_last_switch_error = ret;
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "UVC stream resolution change failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    ESP_LOGI(TAG, "%s USB role: %s -> %s", force_restart ? "Restarting" : "Switching",
             uvc_role_name(current), uvc_role_name(target));

    /* Phase 1: deinit current role */
    if (current == TEST_USB_ROLE_CDC_DEVICE) {
        ret = test_usb_deinit();
    } else if (current == TEST_USB_ROLE_UVC_HOST) {
        if (target != TEST_USB_ROLE_UVC_HOST) {
            test_uvc_clear_resume_marker();
        }
        ret = test_uvc_deinit();
    }
    s_current_role = TEST_USB_ROLE_NONE;
    if (ret != ESP_OK) {
        s_last_switch_error = ret;
        ESP_LOGE(TAG, "USB role %s deinit failed: %s", uvc_role_name(current), esp_err_to_name(ret));
        return ret;
    }

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
        if (force_restart) {
            ret = test_uvc_set_preferred_resolution(command->uvc_h_res, command->uvc_v_res);
        }
        if (ret == ESP_OK) {
            ret = test_uvc_init();
        }
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
        uvc_role_reset_recovery();
        ESP_LOGI(TAG, "USB role switched to %s", uvc_role_name(target));
    } else {
        s_current_role = TEST_USB_ROLE_NONE;
        ESP_LOGE(TAG, "USB role switch to %s failed, now idle", uvc_role_name(target));
    }
    s_last_switch_error = ret;
    return ret;
}

static void uvc_role_check_health(void)
{
    if (s_current_role != TEST_USB_ROLE_UVC_HOST || s_switch_in_progress) {
        return;
    }

    test_uvc_status_t status = {0};
    if (test_uvc_get_status(&status) != ESP_OK || !status.initialized) {
        return;
    }

    if (status.state == TEST_UVC_STATE_HOST_READY && status.device_connected && !status.stream_started) {
        s_switch_in_progress = true;
        s_last_switch_error = test_uvc_run();
        s_switch_in_progress = false;
        if (s_last_switch_error == ESP_OK) {
            uvc_role_reset_recovery();
        }
        return;
    }

    if ((!status.recovery_required && status.state != TEST_UVC_STATE_FAILED) ||
        s_auto_recovery_exhausted ||
        (!status.device_connected && !status.warm_recovery_expected)) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    if (now_us < s_next_recovery_us) {
        return;
    }

    if (s_auto_recovery_level == TEST_UVC_RECOVERY_NONE) {
        s_auto_recovery_level = status.device_connected ? TEST_UVC_RECOVERY_STREAM
                                                        : TEST_UVC_RECOVERY_ROOT_PORT;
    }

    s_switch_in_progress = true;
    esp_err_t ret = test_uvc_recover(s_auto_recovery_level);
    s_switch_in_progress = false;
    s_last_switch_error = ret;
    if (ret == ESP_OK) {
        uvc_role_reset_recovery();
        return;
    }

    s_auto_recovery_attempt++;
    if (s_auto_recovery_attempt >= TEST_UVC_RECOVERY_MAX_ATTEMPTS) {
        s_auto_recovery_attempt = 0;
        if (s_auto_recovery_level < TEST_UVC_RECOVERY_STACK) {
            s_auto_recovery_level++;
        } else {
            s_auto_recovery_exhausted = true;
            ESP_LOGE(TAG, "UVC automatic recovery exhausted: %s", esp_err_to_name(ret));
            return;
        }
    }
    const uint32_t backoff_shift = s_auto_recovery_attempt > 0 ? s_auto_recovery_attempt - 1 : 0;
    const uint32_t backoff_ms = 500U << backoff_shift;
    s_next_recovery_us = now_us + backoff_ms * 1000LL;
}

static void uvc_role_worker_task(void *arg)
{
    (void)arg;

    test_usb_role_command_t msg;
    while (true) {
        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(TEST_UVC_RECOVERY_POLL_MS)) != pdTRUE) {
            uvc_role_check_health();
            continue;
        }
        s_target_role = msg.target;
        s_switch_in_progress = true;
        uvc_role_do_switch(&msg);
        s_switch_in_progress = false;
        s_target_role = s_current_role;
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
    s_target_role = TEST_USB_ROLE_NONE;
    s_switch_in_progress = false;
    s_last_switch_error = ESP_OK;
    uvc_role_reset_recovery();

    s_queue = xQueueCreate(TEST_USB_ROLE_QUEUE_LEN, sizeof(test_usb_role_command_t));
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

    ESP_LOGI(TAG, "Startup role: None (USB roles start on demand from the UI)");

    if (test_uvc_should_resume_after_reset()) {
        ESP_LOGW(TAG, "Warm reset interrupted UVC streaming, queue automatic UVC recovery");
        esp_err_t ret = test_usb_role_switch(TEST_USB_ROLE_UVC_HOST);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "queue warm-reset UVC recovery failed: %s", esp_err_to_name(ret));
        }
    }

    return ESP_OK;
}

esp_err_t test_usb_role_switch(test_usb_role_t target)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "role module not initialized");
    ESP_RETURN_ON_FALSE(target <= TEST_USB_ROLE_UVC_HOST, ESP_ERR_INVALID_ARG, TAG, "invalid target role");

    const test_usb_role_command_t command = {
        .target = target,
        .force_restart = false,
        .uvc_h_res = 0,
        .uvc_v_res = 0,
    };
    if (xQueueSend(s_queue, &command, 0) != pdTRUE) {
        ESP_LOGW(TAG, "role switch queue full, dropping request");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t test_usb_role_restart_uvc(uint32_t h_res, uint32_t v_res)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "role module not initialized");
    ESP_RETURN_ON_FALSE(h_res > 0 && v_res > 0, ESP_ERR_INVALID_ARG, TAG,
                        "invalid UVC resolution");
    const test_usb_role_command_t command = {
        .target = TEST_USB_ROLE_UVC_HOST,
        .force_restart = true,
        .uvc_h_res = h_res,
        .uvc_v_res = v_res,
    };
    if (xQueueSend(s_queue, &command, 0) != pdTRUE) {
        ESP_LOGW(TAG, "UVC restart queue full, dropping request");
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
        .target_role = s_target_role,
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
        status.uvc_frames_dropped = uvc_status.frames_dropped;
        status.uvc_state = uvc_status.state;
        status.uvc_recovery_level = uvc_status.recovery_level;
        status.uvc_recovery_attempts = uvc_status.recovery_attempts;
        status.uvc_recovery_required = uvc_status.recovery_required;
        status.uvc_warm_recovery_expected = uvc_status.warm_recovery_expected;
        status.uvc_last_error = uvc_status.last_error;
    }

    *out_status = status;
    return ESP_OK;
}
