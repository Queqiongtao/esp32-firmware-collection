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
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "test_config.h"
#include "test_usb.h"
#include "test_uvc.h"

static const char *TAG = "test_usb_role";

#define TEST_USB_ROLE_QUEUE_LEN          4
#define TEST_USB_ROLE_TASK_STACK_SIZE    8192
#define TEST_USB_ROLE_TASK_PRIORITY      3
#define TEST_USB_ROLE_TASK_CORE_ID       1
#define TEST_USB_ROLE_DEINIT_TIMEOUT_MS  15000
#define TEST_USB_ROLE_WORKER_EXIT_MS     2000

#define USB_ROLE_COMMAND_DONE_BIT BIT0
#define USB_ROLE_WORKER_EXITED_BIT BIT1

typedef struct {
    test_usb_role_t target;
    bool force_restart;
    bool shutdown;
    uint32_t uvc_h_res;
    uint32_t uvc_v_res;
} test_usb_role_command_t;

static QueueHandle_t s_queue;
static EventGroupHandle_t s_events;
static TaskHandle_t s_task_handle;
static test_usb_role_t s_current_role = TEST_USB_ROLE_NONE;
static test_usb_role_t s_target_role = TEST_USB_ROLE_NONE;
static bool s_switch_in_progress;
static bool s_controller_released = true;
static test_usb_role_fault_t s_fault = TEST_USB_ROLE_FAULT_NONE;
static esp_err_t s_last_switch_error = ESP_OK;
static esp_err_t s_shutdown_result = ESP_OK;
static bool s_initialized;
static test_module_meta_t s_module = {
    .state = TEST_MODULE_STATE_UNINITIALIZED,
    .last_error = ESP_OK,
};
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;

static test_uvc_recovery_level_t s_auto_recovery_level = TEST_UVC_RECOVERY_NONE;
static uint32_t s_auto_recovery_attempt;
static int64_t s_next_recovery_us;
static bool s_auto_recovery_exhausted;

static bool usb_role_is_available(test_usb_role_t role)
{
    switch (role) {
    case TEST_USB_ROLE_NONE:
        return true;
    case TEST_USB_ROLE_CDC_DEVICE:
#if CONFIG_EXAMPLE_ENABLE_TEST_USB
        return true;
#else
        return false;
#endif
    case TEST_USB_ROLE_UVC_HOST:
#if CONFIG_EXAMPLE_ENABLE_TEST_UVC
        return true;
#else
        return false;
#endif
    default:
        return false;
    }
}

static const char *usb_role_name(test_usb_role_t role)
{
    switch (role) {
    case TEST_USB_ROLE_CDC_DEVICE: return "CDC Device";
    case TEST_USB_ROLE_UVC_HOST:   return "UVC Host";
    default:                       return "None";
    }
}

static void usb_role_update_meta_locked(test_module_state_t state, esp_err_t error)
{
    s_module.state = state;
    s_module.last_error = error;
    s_module.updated_at_us = esp_timer_get_time();
    s_last_switch_error = error;
}

static void usb_role_begin_operation(test_usb_role_t target)
{
    portENTER_CRITICAL(&s_status_lock);
    s_target_role = target;
    s_switch_in_progress = true;
    usb_role_update_meta_locked(TEST_MODULE_STATE_INITIALIZING, ESP_OK);
    portEXIT_CRITICAL(&s_status_lock);
}

static void usb_role_finish_operation(esp_err_t error)
{
    portENTER_CRITICAL(&s_status_lock);
    s_switch_in_progress = false;
    s_target_role = s_current_role;
    s_last_switch_error = error;
    if (error == ESP_OK) {
        usb_role_update_meta_locked(s_current_role == TEST_USB_ROLE_NONE ? TEST_MODULE_STATE_READY
                                                                          : TEST_MODULE_STATE_RUNNING,
                                    ESP_OK);
    } else {
        usb_role_update_meta_locked(TEST_MODULE_STATE_ERROR, error);
    }
    portEXIT_CRITICAL(&s_status_lock);
}

static void usb_role_set_fault(test_usb_role_fault_t fault, esp_err_t error,
                               test_usb_role_t owner, bool controller_released)
{
    portENTER_CRITICAL(&s_status_lock);
    s_current_role = owner;
    s_controller_released = controller_released;
    s_fault = fault;
    usb_role_update_meta_locked(TEST_MODULE_STATE_ERROR, error);
    portEXIT_CRITICAL(&s_status_lock);
}

static void usb_role_set_active(test_usb_role_t role)
{
    portENTER_CRITICAL(&s_status_lock);
    s_current_role = role;
    s_controller_released = role == TEST_USB_ROLE_NONE;
    s_fault = TEST_USB_ROLE_FAULT_NONE;
    usb_role_update_meta_locked(role == TEST_USB_ROLE_NONE ? TEST_MODULE_STATE_READY
                                                           : TEST_MODULE_STATE_RUNNING,
                                ESP_OK);
    portEXIT_CRITICAL(&s_status_lock);
}

static bool usb_role_worker_running(void)
{
    portENTER_CRITICAL(&s_status_lock);
    bool running = s_task_handle != NULL;
    portEXIT_CRITICAL(&s_status_lock);
    return running;
}

static esp_err_t usb_role_wait_worker_stopped(void)
{
    const int64_t deadline_us = esp_timer_get_time() + TEST_USB_ROLE_WORKER_EXIT_MS * 1000LL;
    while (usb_role_worker_running()) {
        if (esp_timer_get_time() >= deadline_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return ESP_OK;
}

static bool usb_role_backend_released(test_usb_role_t role)
{
#if CONFIG_EXAMPLE_ENABLE_TEST_USB
    if (role == TEST_USB_ROLE_CDC_DEVICE) {
        test_usb_status_t status = {0};
        return test_usb_get_status(&status) == ESP_OK && status.controller_released;
    }
#endif
#if CONFIG_EXAMPLE_ENABLE_TEST_UVC
    if (role == TEST_USB_ROLE_UVC_HOST) {
        test_uvc_status_t status = {0};
        return test_uvc_get_status(&status) == ESP_OK && status.controller_released;
    }
#endif
    return true;
}

static esp_err_t usb_role_backend_deinit(test_usb_role_t role)
{
#if CONFIG_EXAMPLE_ENABLE_TEST_USB
    if (role == TEST_USB_ROLE_CDC_DEVICE) {
        return test_usb_deinit();
    }
#endif
#if CONFIG_EXAMPLE_ENABLE_TEST_UVC
    if (role == TEST_USB_ROLE_UVC_HOST) {
        return test_uvc_deinit();
    }
#endif
    return ESP_OK;
}

static void usb_role_reset_recovery(void)
{
    s_auto_recovery_level = TEST_UVC_RECOVERY_NONE;
    s_auto_recovery_attempt = 0;
    s_next_recovery_us = 0;
    s_auto_recovery_exhausted = false;
}

/* Execute a switch in the single role-worker context. A non-OK deinit is
 * never followed by installing another stack, even if the controller was
 * released, so the cleanup anomaly remains observable. */
static esp_err_t usb_role_do_switch(const test_usb_role_command_t *command)
{
    const test_usb_role_t target = command->target;
    test_usb_role_t current;
    bool controller_released;

    portENTER_CRITICAL(&s_status_lock);
    current = s_current_role;
    controller_released = s_controller_released;
    portEXIT_CRITICAL(&s_status_lock);

    if (!controller_released && current == TEST_USB_ROLE_NONE) {
        usb_role_set_fault(TEST_USB_ROLE_FAULT_OWNERSHIP_UNKNOWN, ESP_ERR_INVALID_STATE,
                           current, false);
        return ESP_ERR_INVALID_STATE;
    }

    if (current == target && !command->force_restart) {
        return ESP_OK;
    }

#if CONFIG_EXAMPLE_ENABLE_TEST_UVC
    if (current == TEST_USB_ROLE_UVC_HOST && target == TEST_USB_ROLE_UVC_HOST &&
        command->force_restart) {
        ESP_LOGI(TAG, "Changing UVC stream resolution to %ux%u",
                 (unsigned)command->uvc_h_res, (unsigned)command->uvc_v_res);
        esp_err_t ret = test_uvc_change_resolution(command->uvc_h_res, command->uvc_v_res);
        if (ret == ESP_OK) {
            usb_role_set_active(TEST_USB_ROLE_UVC_HOST);
        } else {
            usb_role_set_fault(TEST_USB_ROLE_FAULT_INIT_FAILED, ret,
                               TEST_USB_ROLE_UVC_HOST, false);
        }
        return ret;
    }
#endif

    ESP_LOGI(TAG, "Switching USB role: %s -> %s",
             usb_role_name(current), usb_role_name(target));

    if (current != TEST_USB_ROLE_NONE) {
        esp_err_t ret = usb_role_backend_deinit(current);
        const bool released = usb_role_backend_released(current);
        if (!released) {
            ESP_LOGE(TAG, "%s teardown did not release controller[0]: %s",
                     usb_role_name(current), esp_err_to_name(ret));
            usb_role_set_fault(TEST_USB_ROLE_FAULT_OWNERSHIP_UNKNOWN,
                               ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret,
                               current, false);
            return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
        }

        portENTER_CRITICAL(&s_status_lock);
        s_current_role = TEST_USB_ROLE_NONE;
        s_controller_released = true;
        portEXIT_CRITICAL(&s_status_lock);
#if CONFIG_EXAMPLE_ENABLE_TEST_UVC
        if (current == TEST_USB_ROLE_UVC_HOST && target != TEST_USB_ROLE_UVC_HOST) {
            test_uvc_clear_resume_marker();
        }
#endif
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "%s teardown reported an error after releasing controller[0]: %s",
                     usb_role_name(current), esp_err_to_name(ret));
            usb_role_set_fault(TEST_USB_ROLE_FAULT_DEINIT_FAILED, ret,
                               TEST_USB_ROLE_NONE, true);
            return ret;
        }
    }

    if (target == TEST_USB_ROLE_NONE) {
        usb_role_set_active(TEST_USB_ROLE_NONE);
        usb_role_reset_recovery();
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    portENTER_CRITICAL(&s_status_lock);
    s_controller_released = false;
    portEXIT_CRITICAL(&s_status_lock);
#if CONFIG_EXAMPLE_ENABLE_TEST_USB
    if (target == TEST_USB_ROLE_CDC_DEVICE) {
        ret = test_usb_init();
        if (ret == ESP_OK) {
            ret = test_usb_run();
        }
    }
#endif
#if CONFIG_EXAMPLE_ENABLE_TEST_UVC
    if (target == TEST_USB_ROLE_UVC_HOST) {
        if (command->force_restart) {
            ret = test_uvc_set_preferred_resolution(command->uvc_h_res, command->uvc_v_res);
        }
        if (ret == ESP_OK) {
            ret = test_uvc_init();
        }
        if (ret == ESP_OK) {
            esp_err_t run_ret = test_uvc_run();
            if (run_ret != ESP_OK) {
                ESP_LOGW(TAG, "UVC Host ready without a stream (camera may be absent): %s",
                         esp_err_to_name(run_ret));
            }
        }
    }
#endif

    if (ret == ESP_OK) {
        usb_role_set_active(target);
        usb_role_reset_recovery();
        ESP_LOGI(TAG, "USB role switched to %s", usb_role_name(target));
        return ESP_OK;
    }

    ESP_LOGE(TAG, "%s initialization failed: %s", usb_role_name(target), esp_err_to_name(ret));
    esp_err_t rollback_ret = usb_role_backend_deinit(target);
    const bool released = usb_role_backend_released(target);
    if (!released) {
        usb_role_set_fault(TEST_USB_ROLE_FAULT_OWNERSHIP_UNKNOWN, ret, target, false);
        ESP_LOGE(TAG, "%s rollback did not release controller[0]: %s",
                 usb_role_name(target), esp_err_to_name(rollback_ret));
    } else {
        usb_role_set_fault(rollback_ret == ESP_OK ? TEST_USB_ROLE_FAULT_INIT_FAILED
                                                  : TEST_USB_ROLE_FAULT_ROLLBACK_FAILED,
                           ret, TEST_USB_ROLE_NONE, true);
    }
    return ret;
}

static void usb_role_check_health(void)
{
#if !CONFIG_EXAMPLE_ENABLE_TEST_UVC
    return;
#else
    portENTER_CRITICAL(&s_status_lock);
    bool should_check = s_current_role == TEST_USB_ROLE_UVC_HOST && !s_switch_in_progress;
    portEXIT_CRITICAL(&s_status_lock);
    if (!should_check) {
        return;
    }

    test_uvc_status_t status = {0};
    if (test_uvc_get_status(&status) != ESP_OK || !status.initialized) {
        return;
    }

    if (status.state == TEST_UVC_STATE_HOST_READY && status.device_connected && !status.stream_started) {
        usb_role_begin_operation(TEST_USB_ROLE_UVC_HOST);
        esp_err_t ret = test_uvc_run();
        if (ret == ESP_OK) {
            usb_role_set_active(TEST_USB_ROLE_UVC_HOST);
        }
        usb_role_finish_operation(ret);
        if (ret == ESP_OK) {
            usb_role_reset_recovery();
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

    usb_role_begin_operation(TEST_USB_ROLE_UVC_HOST);
    esp_err_t ret = test_uvc_recover(s_auto_recovery_level);
    if (ret != ESP_OK && usb_role_backend_released(TEST_USB_ROLE_UVC_HOST)) {
        test_uvc_status_t failed_status = {0};
        if (test_uvc_get_status(&failed_status) == ESP_OK && !failed_status.initialized) {
            usb_role_set_fault(TEST_USB_ROLE_FAULT_DEINIT_FAILED, ret,
                               TEST_USB_ROLE_NONE, true);
        }
    }
    if (ret == ESP_OK) {
        usb_role_set_active(TEST_USB_ROLE_UVC_HOST);
    }
    usb_role_finish_operation(ret);
    if (ret == ESP_OK) {
        usb_role_reset_recovery();
        return;
    }

    if (s_auto_recovery_level == TEST_UVC_RECOVERY_STREAM && ret == ESP_ERR_TIMEOUT) {
        s_auto_recovery_level = TEST_UVC_RECOVERY_ROOT_PORT;
        s_auto_recovery_attempt = 0;
        s_next_recovery_us = now_us + 500U * 1000LL;
        ESP_LOGW(TAG, "UVC stream recovery timed out; escalate directly to root-port recovery");
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
    s_next_recovery_us = now_us + (500U << backoff_shift) * 1000LL;
#endif
}

static void usb_role_worker_task(void *arg)
{
    (void)arg;
    test_usb_role_command_t command;

    while (true) {
        if (xQueueReceive(s_queue, &command, pdMS_TO_TICKS(TEST_UVC_RECOVERY_POLL_MS)) != pdTRUE) {
            usb_role_check_health();
            continue;
        }

        usb_role_begin_operation(command.target);
        esp_err_t ret = usb_role_do_switch(&command);
        usb_role_finish_operation(ret);

        if (command.shutdown) {
            s_shutdown_result = ret;
            xEventGroupSetBits(s_events, USB_ROLE_COMMAND_DONE_BIT);
            portENTER_CRITICAL(&s_status_lock);
            bool released = s_controller_released;
            portEXIT_CRITICAL(&s_status_lock);
            if (ret == ESP_OK && released) {
                break;
            }
        }
    }

    xEventGroupSetBits(s_events, USB_ROLE_WORKER_EXITED_BIT);
    portENTER_CRITICAL(&s_status_lock);
    s_task_handle = NULL;
    portEXIT_CRITICAL(&s_status_lock);
    vTaskDelete(NULL);
}

esp_err_t test_usb_role_init(void)
{
    portENTER_CRITICAL(&s_status_lock);
    bool initialized = s_initialized;
    portEXIT_CRITICAL(&s_status_lock);
    if (initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(s_queue == NULL && s_events == NULL && s_task_handle == NULL,
                        ESP_ERR_INVALID_STATE, TAG, "previous role teardown incomplete");
    ESP_LOGI(TAG, "Initializing USB role management framework");

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        portENTER_CRITICAL(&s_status_lock);
        s_module.state = TEST_MODULE_STATE_ERROR;
        s_module.last_error = ESP_ERR_NO_MEM;
        s_module.updated_at_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_status_lock);
        return ESP_ERR_NO_MEM;
    }
    s_queue = xQueueCreate(TEST_USB_ROLE_QUEUE_LEN, sizeof(test_usb_role_command_t));
    if (s_queue == NULL) {
        vEventGroupDelete(s_events);
        s_events = NULL;
        portENTER_CRITICAL(&s_status_lock);
        s_module.state = TEST_MODULE_STATE_ERROR;
        s_module.last_error = ESP_ERR_NO_MEM;
        s_module.updated_at_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_status_lock);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(usb_role_worker_task,
                                                      "usb_role",
                                                      TEST_USB_ROLE_TASK_STACK_SIZE,
                                                      NULL,
                                                      TEST_USB_ROLE_TASK_PRIORITY,
                                                      &s_task_handle,
                                                      TEST_USB_ROLE_TASK_CORE_ID);
    if (task_created != pdTRUE) {
        vQueueDelete(s_queue);
        vEventGroupDelete(s_events);
        s_queue = NULL;
        s_events = NULL;
        portENTER_CRITICAL(&s_status_lock);
        s_module.state = TEST_MODULE_STATE_ERROR;
        s_module.last_error = ESP_ERR_NO_MEM;
        s_module.updated_at_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_status_lock);
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_status_lock);
    s_current_role = TEST_USB_ROLE_NONE;
    s_target_role = TEST_USB_ROLE_NONE;
    s_switch_in_progress = false;
    s_controller_released = true;
    s_fault = TEST_USB_ROLE_FAULT_NONE;
    s_last_switch_error = ESP_OK;
    s_initialized = true;
    s_module.generation++;
    usb_role_update_meta_locked(TEST_MODULE_STATE_READY, ESP_OK);
    portEXIT_CRITICAL(&s_status_lock);
    usb_role_reset_recovery();

    ESP_LOGI(TAG, "Startup role: None (CDC=%s UVC=%s)",
             usb_role_is_available(TEST_USB_ROLE_CDC_DEVICE) ? "available" : "disabled",
             usb_role_is_available(TEST_USB_ROLE_UVC_HOST) ? "available" : "disabled");

#if CONFIG_EXAMPLE_ENABLE_TEST_UVC
    if (test_uvc_should_resume_after_reset()) {
        ESP_LOGW(TAG, "Warm reset interrupted UVC streaming, queue automatic UVC recovery");
        esp_err_t ret = test_usb_role_switch(TEST_USB_ROLE_UVC_HOST);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "queue warm-reset UVC recovery failed: %s", esp_err_to_name(ret));
        }
    }
#endif
    return ESP_OK;
}

esp_err_t test_usb_role_deinit(void)
{
    portENTER_CRITICAL(&s_status_lock);
    bool initialized = s_initialized;
    bool released = s_controller_released;
    portEXIT_CRITICAL(&s_status_lock);
    if (!initialized && s_queue == NULL && s_events == NULL) {
        return ESP_OK;
    }

    if (usb_role_worker_running()) {
        const test_usb_role_command_t command = {
            .target = TEST_USB_ROLE_NONE,
            .shutdown = true,
        };
        xEventGroupClearBits(s_events, USB_ROLE_COMMAND_DONE_BIT | USB_ROLE_WORKER_EXITED_BIT);
        ESP_RETURN_ON_FALSE(xQueueSend(s_queue, &command, 0) == pdTRUE,
                            ESP_ERR_NO_MEM, TAG, "role queue full during deinit");
        EventBits_t bits = xEventGroupWaitBits(s_events, USB_ROLE_COMMAND_DONE_BIT,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(TEST_USB_ROLE_DEINIT_TIMEOUT_MS));
        if ((bits & USB_ROLE_COMMAND_DONE_BIT) == 0) {
            return ESP_ERR_TIMEOUT;
        }
        if (s_shutdown_result != ESP_OK) {
            return s_shutdown_result;
        }
        bits = xEventGroupWaitBits(s_events, USB_ROLE_WORKER_EXITED_BIT,
                                   pdFALSE, pdFALSE,
                                   pdMS_TO_TICKS(TEST_USB_ROLE_WORKER_EXIT_MS));
        if ((bits & USB_ROLE_WORKER_EXITED_BIT) == 0) {
            return ESP_ERR_TIMEOUT;
        }
        ESP_RETURN_ON_ERROR(usb_role_wait_worker_stopped(), TAG,
                            "role worker did not finish publishing its exit");
    }

    portENTER_CRITICAL(&s_status_lock);
    released = s_controller_released;
    portEXIT_CRITICAL(&s_status_lock);
    ESP_RETURN_ON_FALSE(released, ESP_ERR_INVALID_STATE, TAG,
                        "controller[0] release is not confirmed");

    vQueueDelete(s_queue);
    vEventGroupDelete(s_events);
    s_queue = NULL;
    s_events = NULL;
    portENTER_CRITICAL(&s_status_lock);
    s_initialized = false;
    s_current_role = TEST_USB_ROLE_NONE;
    s_target_role = TEST_USB_ROLE_NONE;
    s_switch_in_progress = false;
    s_fault = TEST_USB_ROLE_FAULT_NONE;
    usb_role_update_meta_locked(TEST_MODULE_STATE_UNINITIALIZED, ESP_OK);
    portEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}

esp_err_t test_usb_role_get_capabilities(test_usb_role_capabilities_t *out_capabilities)
{
    ESP_RETURN_ON_FALSE(out_capabilities != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "out_capabilities is NULL");
    *out_capabilities = (test_usb_role_capabilities_t) {
        .cdc_device_available = usb_role_is_available(TEST_USB_ROLE_CDC_DEVICE),
        .uvc_host_available = usb_role_is_available(TEST_USB_ROLE_UVC_HOST),
    };
    return ESP_OK;
}

esp_err_t test_usb_role_switch(test_usb_role_t target)
{
    ESP_RETURN_ON_FALSE(target >= TEST_USB_ROLE_NONE && target <= TEST_USB_ROLE_UVC_HOST,
                        ESP_ERR_INVALID_ARG, TAG, "invalid target role");
    ESP_RETURN_ON_FALSE(usb_role_is_available(target), ESP_ERR_NOT_SUPPORTED, TAG,
                        "%s capability is not compiled in", usb_role_name(target));

    portENTER_CRITICAL(&s_status_lock);
    bool initialized = s_initialized;
    bool ownership_unknown = s_fault == TEST_USB_ROLE_FAULT_OWNERSHIP_UNKNOWN;
    portEXIT_CRITICAL(&s_status_lock);
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, TAG, "role module not initialized");
    ESP_RETURN_ON_FALSE(!ownership_unknown || target == TEST_USB_ROLE_NONE,
                        ESP_ERR_INVALID_STATE, TAG,
                        "controller ownership unknown; only teardown retry is allowed");

    const test_usb_role_command_t command = { .target = target };
    if (xQueueSend(s_queue, &command, 0) != pdTRUE) {
        ESP_LOGW(TAG, "role switch queue full, dropping request");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t test_usb_role_restart_uvc(uint32_t h_res, uint32_t v_res)
{
    ESP_RETURN_ON_FALSE(usb_role_is_available(TEST_USB_ROLE_UVC_HOST),
                        ESP_ERR_NOT_SUPPORTED, TAG, "UVC capability is not compiled in");
    ESP_RETURN_ON_FALSE(h_res > 0 && v_res > 0, ESP_ERR_INVALID_ARG, TAG,
                        "invalid UVC resolution");
    portENTER_CRITICAL(&s_status_lock);
    bool initialized = s_initialized;
    bool ownership_unknown = s_fault == TEST_USB_ROLE_FAULT_OWNERSHIP_UNKNOWN;
    portEXIT_CRITICAL(&s_status_lock);
    ESP_RETURN_ON_FALSE(initialized && !ownership_unknown, ESP_ERR_INVALID_STATE, TAG,
                        "role module unavailable for UVC restart");

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
    portENTER_CRITICAL(&s_status_lock);
    test_usb_role_t role = s_initialized ? s_current_role : TEST_USB_ROLE_NONE;
    portEXIT_CRITICAL(&s_status_lock);
    return role;
}

esp_err_t test_usb_role_get_status(test_usb_role_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != NULL, ESP_ERR_INVALID_ARG, TAG, "out_status is NULL");

    portENTER_CRITICAL(&s_status_lock);
    test_usb_role_status_t status = {
        .module = s_module,
        .current_role = s_current_role,
        .target_role = s_target_role,
        .switch_in_progress = s_switch_in_progress,
        .cdc_available = usb_role_is_available(TEST_USB_ROLE_CDC_DEVICE),
        .uvc_available = usb_role_is_available(TEST_USB_ROLE_UVC_HOST),
        .controller_released = s_controller_released,
        .faulted = s_fault != TEST_USB_ROLE_FAULT_NONE,
        .fault = s_fault,
        .last_switch_error = s_last_switch_error,
    };
    portEXIT_CRITICAL(&s_status_lock);

#if CONFIG_EXAMPLE_ENABLE_TEST_USB
    test_usb_status_t cdc_status = {0};
    if (test_usb_get_status(&cdc_status) == ESP_OK) {
        status.cdc_attached = cdc_status.attached;
        status.cdc_rx_packets = cdc_status.rx_packets;
        status.cdc_tx_packets = cdc_status.tx_packets;
    }
#endif

#if CONFIG_EXAMPLE_ENABLE_TEST_UVC
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
        status.uvc_auto_downgraded = uvc_status.auto_downgraded;
        status.uvc_auto_downgrade_count = uvc_status.auto_downgrade_count;
        status.uvc_last_error = uvc_status.last_error;
    }
#endif

    *out_status = status;
    return ESP_OK;
}
