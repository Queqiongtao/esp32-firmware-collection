/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "test_wireless.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "espressif__esp_hosted/host/esp_hosted.h"
#include "espressif__esp_hosted/host/esp_hosted_event.h"
#include "lwip/inet.h"
#include "test_config.h"

/* esp_hosted 2.12.x has no public transport-ready getter. This pinned private
 * symbol distinguishes a reused process-lifetime link because reconnecting an
 * already-up transport does not emit ESP_HOSTED_EVENT_TRANSPORT_UP. */
extern uint8_t is_transport_tx_ready(void);

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED && CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#endif

static const char *TAG = "test_wireless";

#if CONFIG_EXAMPLE_WIRELESS_ENABLE_WIFI
#define WIRELESS_WIFI_FEATURE_ENABLED           1
#define WIRELESS_WIFI_MAXIMUM_RETRY             CONFIG_EXAMPLE_WIRELESS_MAXIMUM_RETRY
#else
#define WIRELESS_WIFI_FEATURE_ENABLED           0
#define WIRELESS_WIFI_MAXIMUM_RETRY             UINT32_MAX
#endif

#if CONFIG_EXAMPLE_WIRELESS_ENABLE_BLE
#define WIRELESS_BLE_FEATURE_ENABLED            1
#else
#define WIRELESS_BLE_FEATURE_ENABLED            0
#endif

#if WIRELESS_BLE_FEATURE_ENABLED && CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED && CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
#define WIRELESS_BLE_HOSTED_ENABLED             1
#else
#define WIRELESS_BLE_HOSTED_ENABLED             0
#endif

#define WIRELESS_HOSTED_EVENT_TRANSPORT_UP_BIT  BIT0
#define WIRELESS_HOSTED_EVENT_CP_INIT_BIT       BIT1
#define WIRELESS_HOSTED_EVENT_FAILURE_BIT       BIT2
#define WIRELESS_HOSTED_EVENT_DOWN_BIT          BIT3
#define WIRELESS_HOSTED_READY_BITS              (WIRELESS_HOSTED_EVENT_TRANSPORT_UP_BIT | WIRELESS_HOSTED_EVENT_CP_INIT_BIT)
#define WIRELESS_HOSTED_WAIT_READY_MS           5000
#define WIRELESS_CALLBACK_DRAIN_TIMEOUT_MS      2000
#define WIRELESS_BLE_STOP_TIMEOUT_MS            2000
#define WIRELESS_RECONNECT_BASE_DELAY_MS        500
#define WIRELESS_RECONNECT_MAX_DELAY_MS         8000
#define WIRELESS_WIFI_COMMAND_IDLE_BIT          BIT0
#define WIRELESS_EXPECTED_CP_TARGET             "esp32c6"
#define WIRELESS_EXPECTED_RESET_GPIO            54
#define WIRELESS_BLE_DEVICE_NAME                "JC1060P470-BLE"

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static test_wireless_status_t s_status;
static test_wireless_scan_snapshot_t s_scan_snapshot;
static uint32_t s_status_generation;
static uint32_t s_scan_generation;
static uint32_t s_activity_users;
static bool s_callbacks_enabled;
static bool s_actions_enabled;

static bool s_event_loop_ready;
static bool s_netif_ready;
static bool s_hosted_ready;
static bool s_hosted_handler_registered;
static bool s_wifi_driver_ready;
static bool s_wifi_driver_owned;
static bool s_wifi_event_registered;
static bool s_ip_event_registered;
static bool s_wifi_started;
static bool s_wifi_connect_requested;
static bool s_suppress_next_disconnect;
static bool s_wifi_teardown = true;
static bool s_wifi_command_active;
static uint32_t s_wifi_request_generation;
static uint32_t s_wifi_connected_generation;
static uint32_t s_reconnect_timer_generation;
static bool s_ble_controller_ready;
static bool s_ble_host_ready;
#if WIRELESS_BLE_HOSTED_ENABLED
static bool s_ble_controller_enabled;
static bool s_ble_host_task_created;
static bool s_ble_host_task_started;
static bool s_ble_host_synced;
static bool s_ble_connected;
static uint8_t s_ble_own_addr_type;
static uint8_t s_ble_test_value;
static uint16_t s_ble_test_value_handle;
#endif
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_hosted_evt_instance;
static esp_event_handler_instance_t s_wifi_evt_instance;
static esp_event_handler_instance_t s_ip_evt_instance;
static EventGroupHandle_t s_hosted_event_group;
static TimerHandle_t s_reconnect_timer;
static StaticSemaphore_t s_wifi_command_mutex_storage;
static SemaphoreHandle_t s_wifi_command_mutex;
static StaticEventGroup_t s_wifi_command_idle_group_storage;
static EventGroupHandle_t s_wifi_command_idle_group;

#if WIRELESS_BLE_HOSTED_ENABLED
extern void ble_store_config_init(void);

static const ble_uuid128_t s_ble_test_service_uuid =
    BLE_UUID128_INIT(0x70, 0x04, 0x60, 0x10, 0x47, 0x60, 0x60, 0x10,
                     0x99, 0x99, 0x43, 0x95, 0x12, 0x2f, 0x46, 0x59);
static const ble_uuid128_t s_ble_test_chr_uuid =
    BLE_UUID128_INIT(0x71, 0x04, 0x60, 0x10, 0x47, 0x60, 0x60, 0x10,
                     0x99, 0x99, 0x43, 0x95, 0x12, 0x2f, 0x46, 0x59);
#endif

static int64_t wireless_now_us(void)
{
    return esp_timer_get_time();
}

static void wireless_copy_string(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }

    size_t length = src != NULL ? strnlen(src, dst_size - 1) : 0;
    if (length > 0) {
        memcpy(dst, src, length);
    }
    dst[length] = '\0';
}

static void wireless_commit_locked(int64_t now_us)
{
    if (now_us > s_status.module.updated_at_us) {
        s_status.module.updated_at_us = now_us;
    }
    s_status.module.last_error = s_status.last_error;
}

static void wireless_advance_lifecycle_locked(void)
{
    s_status_generation++;
    if (s_status_generation == 0) {
        s_status_generation++;
    }
    s_status.module.generation = s_status_generation;
}

static void wireless_set_hint(const char *hint)
{
    int64_t now_us = wireless_now_us();
    taskENTER_CRITICAL(&s_state_lock);
    wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), hint);
    wireless_commit_locked(now_us);
    taskEXIT_CRITICAL(&s_state_lock);
}

static void wireless_set_error(esp_err_t err, const char *hint)
{
    int64_t now_us = wireless_now_us();
    taskENTER_CRITICAL(&s_state_lock);
    s_status.last_error = err;
    wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), hint);
    wireless_commit_locked(now_us);
    taskEXIT_CRITICAL(&s_state_lock);
}

static void wireless_set_transport_error(esp_err_t err, const char *hint)
{
    int64_t now_us = wireless_now_us();
    taskENTER_CRITICAL(&s_state_lock);
    s_status.transport_confirmed = false;
    s_status.transport_state = TEST_WIRELESS_TRANSPORT_ERROR;
    if (s_status.wifi_feature_enabled) {
        s_wifi_connected_generation = 0;
        s_status.wifi_connected = false;
        s_status.wifi_rssi = 0;
        wireless_copy_string(s_status.current_ip, sizeof(s_status.current_ip), "--");
        s_status.wifi_state = TEST_WIRELESS_WIFI_ERROR;
    }
    s_status.last_error = err;
    s_status.module.state = TEST_MODULE_STATE_ERROR;
    wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), hint);
    wireless_commit_locked(now_us);
    taskEXIT_CRITICAL(&s_state_lock);
}

static bool wireless_activity_enter(bool action)
{
    bool allowed;
    taskENTER_CRITICAL(&s_state_lock);
    allowed = action ? (s_actions_enabled && s_status.initialized) : s_callbacks_enabled;
    if (allowed) {
        s_activity_users++;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return allowed;
}

static void wireless_activity_exit(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    if (s_activity_users > 0) {
        s_activity_users--;
    }
    taskEXIT_CRITICAL(&s_state_lock);
}

static esp_err_t wireless_wait_activity_idle(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while (true) {
        uint32_t users;
        taskENTER_CRITICAL(&s_state_lock);
        users = s_activity_users;
        taskEXIT_CRITICAL(&s_state_lock);
        if (users == 0) {
            return ESP_OK;
        }
        if ((xTaskGetTickCount() - start) >= timeout_ticks) {
            ESP_LOGE(TAG, "Timed out waiting for %" PRIu32 " wireless callbacks/actions", users);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t wireless_require_initialized(void)
{
    bool initialized;
    taskENTER_CRITICAL(&s_state_lock);
    initialized = s_status.initialized && s_actions_enabled;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, TAG,
                        "wireless not initialized, call test_wireless_init first");
    return ESP_OK;
}

static esp_err_t wireless_require_transport_confirmed(void)
{
    bool confirmed;
    taskENTER_CRITICAL(&s_state_lock);
    confirmed = s_status.transport_confirmed;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_RETURN_ON_FALSE(confirmed, ESP_ERR_INVALID_STATE, TAG, "hosted transport not confirmed");
    return ESP_OK;
}

static void wireless_record_first_error(esp_err_t *aggregate, esp_err_t error)
{
    if (*aggregate == ESP_OK && error != ESP_OK) {
        *aggregate = error;
    }
}

static esp_err_t wireless_prepare_wifi_command_sync(void)
{
    if (s_wifi_command_mutex == NULL) {
        s_wifi_command_mutex = xSemaphoreCreateMutexStatic(&s_wifi_command_mutex_storage);
    }
    ESP_RETURN_ON_FALSE(s_wifi_command_mutex != NULL, ESP_ERR_NO_MEM, TAG,
                        "create WiFi command mutex failed");

    if (s_wifi_command_idle_group == NULL) {
        s_wifi_command_idle_group =
            xEventGroupCreateStatic(&s_wifi_command_idle_group_storage);
        if (s_wifi_command_idle_group != NULL) {
            xEventGroupSetBits(s_wifi_command_idle_group, WIRELESS_WIFI_COMMAND_IDLE_BIT);
        }
    }
    ESP_RETURN_ON_FALSE(s_wifi_command_idle_group != NULL, ESP_ERR_NO_MEM, TAG,
                        "create WiFi command event group failed");
    return ESP_OK;
}

/* The mutex protects command decisions. The active flag keeps ESP WiFi calls
 * serialized while allowing synchronous event callbacks to take the mutex. */
static esp_err_t wireless_wifi_command_claim(bool wait)
{
    ESP_RETURN_ON_FALSE(s_wifi_command_mutex != NULL && s_wifi_command_idle_group != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "WiFi command sync not ready");

    while (true) {
        TickType_t mutex_wait = wait ? portMAX_DELAY : 0;
        if (xSemaphoreTake(s_wifi_command_mutex, mutex_wait) != pdTRUE) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!s_wifi_command_active) {
            s_wifi_command_active = true;
            xEventGroupClearBits(s_wifi_command_idle_group, WIRELESS_WIFI_COMMAND_IDLE_BIT);
            xSemaphoreGive(s_wifi_command_mutex);
            return ESP_OK;
        }
        xSemaphoreGive(s_wifi_command_mutex);

        if (!wait) {
            return ESP_ERR_INVALID_STATE;
        }
        (void)xEventGroupWaitBits(s_wifi_command_idle_group,
                                  WIRELESS_WIFI_COMMAND_IDLE_BIT,
                                  pdFALSE, pdTRUE, portMAX_DELAY);
    }
}

static void wireless_wifi_command_release(void)
{
    if (s_wifi_command_mutex == NULL || s_wifi_command_idle_group == NULL) {
        return;
    }
    if (xSemaphoreTake(s_wifi_command_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    s_wifi_command_active = false;
    xEventGroupSetBits(s_wifi_command_idle_group, WIRELESS_WIFI_COMMAND_IDLE_BIT);
    xSemaphoreGive(s_wifi_command_mutex);
}

static uint32_t wireless_next_wifi_request_generation_locked(void)
{
    s_wifi_request_generation++;
    if (s_wifi_request_generation == 0) {
        s_wifi_request_generation++;
    }
    return s_wifi_request_generation;
}

static bool wireless_wifi_action_allowed_locked(bool require_transport)
{
    bool allowed;
    taskENTER_CRITICAL(&s_state_lock);
    allowed = s_status.initialized && s_actions_enabled && s_callbacks_enabled &&
              (!require_transport || s_status.transport_confirmed);
    taskEXIT_CRITICAL(&s_state_lock);
    return allowed && !s_wifi_teardown && s_wifi_started;
}

static bool wireless_wifi_reconnect_allowed_locked(uint32_t generation)
{
    bool allowed;
    taskENTER_CRITICAL(&s_state_lock);
    allowed = s_status.initialized && s_callbacks_enabled &&
              s_status.transport_confirmed && s_wifi_connect_requested;
    taskEXIT_CRITICAL(&s_state_lock);
    return allowed && !s_wifi_teardown && s_wifi_started &&
           generation != 0 && generation == s_wifi_request_generation;
}

static bool wireless_wifi_ssid_matches_locked(const uint8_t *ssid, size_t ssid_len)
{
    const size_t expected_len = strnlen(s_status.current_ssid,
                                        sizeof(s_status.current_ssid));
    return ssid != NULL && ssid_len == expected_len &&
           memcmp(ssid, s_status.current_ssid, expected_len) == 0;
}

static void wireless_stop_reconnect_timer_locked(void)
{
    s_reconnect_timer_generation = 0;
    if (s_reconnect_timer != NULL) {
        (void)xTimerStop(s_reconnect_timer, 0);
    }
}

static wifi_auth_mode_t wireless_auth_mode_threshold(void)
{
#if CONFIG_EXAMPLE_WIRELESS_AUTH_OPEN
    return WIFI_AUTH_OPEN;
#elif CONFIG_EXAMPLE_WIRELESS_AUTH_WEP
    return WIFI_AUTH_WEP;
#elif CONFIG_EXAMPLE_WIRELESS_AUTH_WPA_PSK
    return WIFI_AUTH_WPA_PSK;
#elif CONFIG_EXAMPLE_WIRELESS_AUTH_WPA_WPA2_PSK
    return WIFI_AUTH_WPA_WPA2_PSK;
#elif CONFIG_EXAMPLE_WIRELESS_AUTH_WPA3_PSK
    return WIFI_AUTH_WPA3_PSK;
#elif CONFIG_EXAMPLE_WIRELESS_AUTH_WPA2_WPA3_PSK
    return WIFI_AUTH_WPA2_WPA3_PSK;
#elif CONFIG_EXAMPLE_WIRELESS_AUTH_WAPI_PSK
    return WIFI_AUTH_WAPI_PSK;
#else
    return WIFI_AUTH_WPA2_PSK;
#endif
}

static bool wireless_reason_is_no_ap_found(uint8_t reason)
{
    return reason == WIFI_REASON_NO_AP_FOUND ||
           reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

static bool wireless_auth_mode_is_secured(wifi_auth_mode_t authmode)
{
    return authmode != WIFI_AUTH_OPEN;
}

static void wireless_log_expected_config(void)
{
    ESP_LOGI(TAG, "Expected hosted config: target=%s transport=SDIO slot=%d width=%d clk=%d kHz D0=%d D1=%d D2=%d D3=%d CLK=%d CMD=%d RST=%d",
             WIRELESS_EXPECTED_CP_TARGET, 1, TEST_WIRELESS_SDIO_BUS_WIDTH,
             TEST_WIRELESS_SDIO_CLOCK_KHZ, TEST_WIRELESS_SDIO_PIN_D0,
             TEST_WIRELESS_SDIO_PIN_D1, TEST_WIRELESS_SDIO_PIN_D2,
             TEST_WIRELESS_SDIO_PIN_D3, TEST_WIRELESS_SDIO_PIN_CLK,
             TEST_WIRELESS_SDIO_PIN_CMD, WIRELESS_EXPECTED_RESET_GPIO);
}

static esp_err_t wireless_validate_hosted_config(void)
{
#if !CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE
    ESP_LOGE(TAG, "Hosted config mismatch: expected SDIO host interface");
    wireless_log_expected_config();
    wireless_set_transport_error(ESP_ERR_INVALID_STATE, "cfg mismatch: need c6 sdio");
    return ESP_ERR_INVALID_STATE;
#else
    bool valid = strcmp(CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET, WIRELESS_EXPECTED_CP_TARGET) == 0 &&
                 CONFIG_ESP_HOSTED_SDIO_SLOT == 1 &&
                 CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH == TEST_WIRELESS_SDIO_BUS_WIDTH &&
                 CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ == TEST_WIRELESS_SDIO_CLOCK_KHZ &&
                 CONFIG_ESP_HOSTED_SDIO_PIN_D0 == TEST_WIRELESS_SDIO_PIN_D0 &&
                 CONFIG_ESP_HOSTED_SDIO_PIN_D1 == TEST_WIRELESS_SDIO_PIN_D1 &&
                 CONFIG_ESP_HOSTED_SDIO_PIN_D2 == TEST_WIRELESS_SDIO_PIN_D2 &&
                 CONFIG_ESP_HOSTED_SDIO_PIN_D3 == TEST_WIRELESS_SDIO_PIN_D3 &&
                 CONFIG_ESP_HOSTED_SDIO_PIN_CLK == TEST_WIRELESS_SDIO_PIN_CLK &&
                 CONFIG_ESP_HOSTED_SDIO_PIN_CMD == TEST_WIRELESS_SDIO_PIN_CMD &&
                 CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE == WIRELESS_EXPECTED_RESET_GPIO;
    if (!valid) {
        ESP_LOGE(TAG, "Hosted SDIO configuration does not match JC1060P470 wiring");
        wireless_log_expected_config();
        wireless_set_transport_error(ESP_ERR_INVALID_STATE, "cfg mismatch: need c6 sdio");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Hosted transport config: SDIO target=%s slot=%d width=%d clk=%d kHz",
             CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET, CONFIG_ESP_HOSTED_SDIO_SLOT,
             CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH, CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ);
    return ESP_OK;
#endif
}

static void wireless_publish_scan(const test_wireless_ap_record_t *records, size_t count,
                                  uint32_t total_count, bool valid, esp_err_t error,
                                  const char *hint)
{
    int64_t now_us = wireless_now_us();
    taskENTER_CRITICAL(&s_state_lock);
    if (!s_status.transport_confirmed) {
        taskEXIT_CRITICAL(&s_state_lock);
        return;
    }
    memset(&s_scan_snapshot, 0, sizeof(s_scan_snapshot));
    s_scan_snapshot.generation = ++s_scan_generation;
    s_scan_snapshot.updated_at_us = now_us;
    s_scan_snapshot.total_count = total_count;
    s_scan_snapshot.record_count = count;
    s_scan_snapshot.valid = valid;
    if (records != NULL && count > 0) {
        memcpy(s_scan_snapshot.records, records, count * sizeof(*records));
    }
    s_status.wifi_ap_count = total_count;
    s_status.wifi_scan_results_valid = valid;
    s_status.wifi_scan_generation = s_scan_snapshot.generation;
    s_status.wifi_scan_updated_at_us = now_us;
    s_status.last_error = error;
    wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), hint);
    wireless_commit_locked(now_us);
    taskEXIT_CRITICAL(&s_state_lock);
}

static esp_err_t wireless_update_scan_results(void)
{
    uint16_t ap_count = 0;
    uint16_t fetch_count = TEST_WIRELESS_SCAN_RESULT_MAX;
    wifi_ap_record_t ap_info[TEST_WIRELESS_SCAN_RESULT_MAX] = {0};
    test_wireless_ap_record_t records[TEST_WIRELESS_SCAN_RESULT_MAX] = {0};

    esp_err_t ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        wireless_publish_scan(NULL, 0, 0, false, ret, "scan get num fail");
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (fetch_count > ap_count) {
        fetch_count = ap_count;
    }
    if (fetch_count > 0) {
        ret = esp_wifi_scan_get_ap_records(&fetch_count, ap_info);
        if (ret != ESP_OK) {
            wireless_publish_scan(NULL, 0, ap_count, false, ret, "scan get rec fail");
            ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    for (size_t i = 0; i < fetch_count; i++) {
        snprintf(records[i].ssid, sizeof(records[i].ssid), "%.*s",
                 (int)sizeof(ap_info[i].ssid), ap_info[i].ssid);
        records[i].rssi = ap_info[i].rssi;
        records[i].secured = wireless_auth_mode_is_secured(ap_info[i].authmode);
    }
    wireless_publish_scan(records, fetch_count, ap_count, true, ESP_OK,
                          ap_count == 0 ? "scan done 0 ap" : "wifi scan done");
    return ESP_OK;
}

static void wireless_record_cp_info(void)
{
    uint32_t cp_chip_id = 0;
    char cp_target_name[TEST_WIRELESS_CP_TARGET_MAX_LEN] = "unknown";
    char cp_fw_version[TEST_WIRELESS_FW_VERSION_MAX_LEN] = "unread";
    esp_hosted_coprocessor_fwver_t fwver = {0};

    esp_err_t cp_ret = esp_hosted_get_cp_info(&cp_chip_id, cp_target_name, sizeof(cp_target_name));
    if (cp_ret != ESP_OK) {
        cp_chip_id = 0;
        wireless_copy_string(cp_target_name, sizeof(cp_target_name), "unknown");
        ESP_LOGW(TAG, "esp_hosted_get_cp_info failed: %s", esp_err_to_name(cp_ret));
    }

    esp_err_t fw_ret = esp_hosted_get_coprocessor_fwversion(&fwver);
    if (fw_ret == ESP_OK) {
        snprintf(cp_fw_version, sizeof(cp_fw_version), "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 fwver.major1, fwver.minor1, fwver.patch1);
    } else {
        ESP_LOGW(TAG, "esp_hosted_get_coprocessor_fwversion failed: %s", esp_err_to_name(fw_ret));
    }

    int64_t now_us = wireless_now_us();
    taskENTER_CRITICAL(&s_state_lock);
    s_status.cp_chip_id = cp_chip_id;
    wireless_copy_string(s_status.cp_target_name, sizeof(s_status.cp_target_name), cp_target_name);
    wireless_copy_string(s_status.cp_fw_version, sizeof(s_status.cp_fw_version), cp_fw_version);
    s_status.hosted_cp_info_error = cp_ret;
    s_status.hosted_fw_version_error = fw_ret;
    s_status.rpc_metadata_ready = fw_ret == ESP_OK;
    wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint),
                         fw_ret == ESP_OK ? "cp verified" : "cp meta fail");
    wireless_commit_locked(now_us);
    taskEXIT_CRITICAL(&s_state_lock);
}

#if WIRELESS_BLE_HOSTED_ENABLED
static int wireless_ble_gap_event(struct ble_gap_event *event, void *arg);

static void wireless_set_ble_error(esp_err_t err, const char *hint)
{
    int64_t now_us = wireless_now_us();
    taskENTER_CRITICAL(&s_state_lock);
    s_status.ble_advertising_enabled = false;
    s_status.ble_connected = s_ble_connected;
    s_status.ble_host_synced = s_ble_host_synced;
    s_status.ble_test_value = s_ble_test_value;
    s_status.ble_state = TEST_WIRELESS_BLE_ERROR;
    s_status.last_error = err;
    wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), hint);
    wireless_commit_locked(now_us);
    taskEXIT_CRITICAL(&s_state_lock);
}

static int wireless_ble_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (!wireless_activity_enter(false)) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    int result = BLE_ATT_ERR_UNLIKELY;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr_handle == s_ble_test_value_handle) {
        uint8_t value;
        taskENTER_CRITICAL(&s_state_lock);
        value = s_ble_test_value;
        taskEXIT_CRITICAL(&s_state_lock);
        result = os_mbuf_append(ctxt->om, &value, sizeof(value)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (attr_handle != s_ble_test_value_handle || OS_MBUF_PKTLEN(ctxt->om) != sizeof(uint8_t)) {
            result = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        } else {
            uint8_t value = 0;
            uint16_t len = 0;
            int rc = ble_hs_mbuf_to_flat(ctxt->om, &value, sizeof(value), &len);
            if (rc == 0 && len == sizeof(value)) {
                int64_t now_us = wireless_now_us();
                taskENTER_CRITICAL(&s_state_lock);
                s_ble_test_value = value;
                s_status.ble_test_value = value;
                s_status.last_error = ESP_OK;
                wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), "ble gatt write");
                wireless_commit_locked(now_us);
                taskEXIT_CRITICAL(&s_state_lock);
                ble_gatts_chr_updated(s_ble_test_value_handle);
                ESP_LOGD(TAG, "BLE test characteristic updated: %u", value);
                result = 0;
            }
        }
    }

    wireless_activity_exit();
    return result;
}

static const struct ble_gatt_svc_def s_ble_gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_ble_test_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_ble_test_chr_uuid.u,
                .access_cb = wireless_ble_gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_ble_test_value_handle,
            },
            {0},
        },
    },
    {0},
};

static esp_err_t wireless_ble_start_advertising(void)
{
    bool synced;
    bool advertising;
    bool connected;
    bool host_ready;
    taskENTER_CRITICAL(&s_state_lock);
    synced = s_ble_host_synced;
    advertising = s_status.ble_advertising_enabled;
    connected = s_ble_connected;
    host_ready = s_ble_host_ready;
    taskEXIT_CRITICAL(&s_state_lock);

    if (!synced || !host_ready) {
        wireless_set_ble_error(ESP_ERR_INVALID_STATE, "ble not synced");
        return ESP_ERR_INVALID_STATE;
    }
    if (advertising) {
        return ESP_OK;
    }
    if (connected) {
        wireless_set_error(ESP_ERR_INVALID_STATE, "ble connected");
        return ESP_ERR_INVALID_STATE;
    }

    struct ble_hs_adv_fields adv_fields = {0};
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.tx_pwr_lvl_is_present = 1;
    adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    adv_fields.name = (uint8_t *)WIRELESS_BLE_DEVICE_NAME;
    adv_fields.name_len = strlen(WIRELESS_BLE_DEVICE_NAME);
    adv_fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        wireless_set_ble_error(ESP_FAIL, rc == BLE_HS_EMSGSIZE ? "ble adv data too large" : "ble adv fields fail");
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return ESP_FAIL;
    }

    struct ble_hs_adv_fields scan_rsp_fields = {0};
    scan_rsp_fields.uuids128 = &s_ble_test_service_uuid;
    scan_rsp_fields.num_uuids128 = 1;
    scan_rsp_fields.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&scan_rsp_fields);
    if (rc != 0) {
        wireless_set_ble_error(ESP_FAIL, rc == BLE_HS_EMSGSIZE ? "ble rsp data too large" : "ble rsp fields fail");
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed: %d", rc);
        return ESP_FAIL;
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_ble_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, wireless_ble_gap_event, NULL);
    if (rc != 0) {
        wireless_set_ble_error(ESP_FAIL, "ble adv start fail");
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
        return ESP_FAIL;
    }

    int64_t now_us = wireless_now_us();
    taskENTER_CRITICAL(&s_state_lock);
    s_status.ble_advertising_enabled = true;
    s_status.ble_connected = false;
    s_status.ble_host_synced = true;
    s_status.ble_test_value = s_ble_test_value;
    s_status.ble_state = TEST_WIRELESS_BLE_ADVERTISING;
    s_status.last_error = ESP_OK;
    wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), "ble advertising");
    wireless_commit_locked(now_us);
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

static esp_err_t wireless_ble_stop_advertising(void)
{
    bool advertising;
    bool host_ready;
    taskENTER_CRITICAL(&s_state_lock);
    advertising = s_status.ble_advertising_enabled;
    host_ready = s_ble_host_ready;
    taskEXIT_CRITICAL(&s_state_lock);

    esp_err_t ret = ESP_OK;
    bool stopped = !advertising;
    if (advertising && !host_ready) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (advertising) {
        int rc = ble_gap_adv_stop();
        stopped = rc == 0 || rc == BLE_HS_EALREADY;
        if (!stopped) {
            ESP_LOGW(TAG, "ble_gap_adv_stop failed: %d", rc);
            ret = ESP_FAIL;
        }
    }

    int64_t now_us = wireless_now_us();
    taskENTER_CRITICAL(&s_state_lock);
    s_status.ble_advertising_enabled = !stopped;
    s_status.ble_connected = s_ble_connected;
    s_status.ble_host_synced = s_ble_host_synced;
    s_status.ble_state = stopped ?
                         (host_ready ? TEST_WIRELESS_BLE_IDLE : TEST_WIRELESS_BLE_NOT_AVAILABLE) :
                         TEST_WIRELESS_BLE_ADVERTISING;
    s_status.last_error = ret;
    wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint),
                         ret == ESP_OK ? "ble adv stop" : "ble adv stop fail");
    wireless_commit_locked(now_us);
    taskEXIT_CRITICAL(&s_state_lock);
    return ret;
}

static int wireless_ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (!wireless_activity_enter(false)) {
        return 0;
    }

    bool restart_advertising = false;
    int64_t now_us = wireless_now_us();
    switch (event->type) {
#if defined(BLE_GAP_EVENT_LINK_ESTAB)
    case BLE_GAP_EVENT_LINK_ESTAB:
#else
    case BLE_GAP_EVENT_CONNECT:
#endif
        taskENTER_CRITICAL(&s_state_lock);
        s_ble_connected = event->connect.status == 0;
        s_status.ble_advertising_enabled = false;
        s_status.ble_connected = s_ble_connected;
        s_status.ble_host_synced = s_ble_host_synced;
        s_status.ble_state = TEST_WIRELESS_BLE_IDLE;
        s_status.last_error = event->connect.status == 0 ? ESP_OK : ESP_FAIL;
        wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint),
                             event->connect.status == 0 ? "ble connected" : "ble conn fail");
        wireless_commit_locked(now_us);
        taskEXIT_CRITICAL(&s_state_lock);
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "BLE connected, handle=%d", event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "BLE connection failed, status=%d", event->connect.status);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        taskENTER_CRITICAL(&s_state_lock);
        s_ble_connected = false;
        s_status.ble_connected = false;
        s_status.ble_advertising_enabled = false;
        s_status.ble_host_synced = s_ble_host_synced;
        s_status.ble_state = TEST_WIRELESS_BLE_IDLE;
        s_status.last_error = ESP_OK;
        restart_advertising = s_callbacks_enabled && s_status.ble_feature_enabled;
        wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), "ble disconnected");
        wireless_commit_locked(now_us);
        taskEXIT_CRITICAL(&s_state_lock);
        ESP_LOGI(TAG, "BLE disconnected, reason=%d", event->disconnect.reason);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        taskENTER_CRITICAL(&s_state_lock);
        s_status.ble_advertising_enabled = false;
        s_status.ble_connected = s_ble_connected;
        s_status.ble_host_synced = s_ble_host_synced;
        s_status.ble_state = TEST_WIRELESS_BLE_IDLE;
        wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), "ble adv complete");
        wireless_commit_locked(now_us);
        taskEXIT_CRITICAL(&s_state_lock);
        ESP_LOGD(TAG, "BLE advertising complete, reason=%d", event->adv_complete.reason);
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        wireless_set_hint(event->subscribe.cur_notify ? "ble notify on" : "ble notify off");
        ESP_LOGD(TAG, "BLE subscription notify=%d", event->subscribe.cur_notify);
        break;

    default:
        break;
    }

    if (restart_advertising) {
        (void)wireless_ble_start_advertising();
    }
    wireless_activity_exit();
    return 0;
}

static void wireless_ble_on_reset(int reason)
{
    if (!wireless_activity_enter(false)) {
        return;
    }
    taskENTER_CRITICAL(&s_state_lock);
    s_ble_host_synced = false;
    taskEXIT_CRITICAL(&s_state_lock);
    wireless_set_ble_error(ESP_FAIL, "ble host reset");
    ESP_LOGE(TAG, "NimBLE reset, reason=%d", reason);
    wireless_activity_exit();
}

static void wireless_ble_on_sync(void)
{
    if (!wireless_activity_enter(false)) {
        return;
    }

    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_ble_own_addr_type);
    }
    if (rc != 0) {
        wireless_set_ble_error(ESP_FAIL, "ble addr fail");
        ESP_LOGE(TAG, "BLE address setup failed: %d", rc);
        wireless_activity_exit();
        return;
    }

    uint8_t addr_val[6] = {0};
    if (ble_hs_id_copy_addr(s_ble_own_addr_type, addr_val, NULL) == 0) {
        ESP_LOGI(TAG, "BLE address: %02x:%02x:%02x:%02x:%02x:%02x",
                 addr_val[5], addr_val[4], addr_val[3], addr_val[2], addr_val[1], addr_val[0]);
    }

    int64_t now_us = wireless_now_us();
    bool advertise;
    taskENTER_CRITICAL(&s_state_lock);
    s_ble_host_synced = true;
    s_status.ble_connected = s_ble_connected;
    s_status.ble_host_synced = true;
    s_status.ble_test_value = s_ble_test_value;
    s_status.ble_state = TEST_WIRELESS_BLE_IDLE;
    s_status.last_error = ESP_OK;
    advertise = s_callbacks_enabled && s_status.ble_feature_enabled;
    wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), "ble ready");
    wireless_commit_locked(now_us);
    taskEXIT_CRITICAL(&s_state_lock);

    if (advertise) {
        (void)wireless_ble_start_advertising();
    }
    wireless_activity_exit();
}

static void wireless_ble_host_task(void *param)
{
    (void)param;
    taskENTER_CRITICAL(&s_state_lock);
    s_ble_host_task_started = true;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    taskENTER_CRITICAL(&s_state_lock);
    s_ble_host_task_started = false;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG, "BLE host task stopped");
    vTaskSuspend(NULL);
}

static esp_err_t wireless_wait_ble_host_started(void)
{
    TickType_t start = xTaskGetTickCount();
    while (true) {
        bool running;
        taskENTER_CRITICAL(&s_state_lock);
        running = s_ble_host_task_started;
        taskEXIT_CRITICAL(&s_state_lock);
        if (running) {
            return ESP_OK;
        }
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(1000)) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t wireless_prepare_ble(void)
{
    if (!WIRELESS_BLE_FEATURE_ENABLED) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(wireless_require_transport_confirmed(), TAG, "hosted transport not ready for BLE");
    esp_err_t ret = esp_hosted_bt_controller_init();
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_hosted_bt_controller_init failed");
    s_ble_controller_ready = true;

    ret = esp_hosted_bt_controller_enable();
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_hosted_bt_controller_enable failed");
    s_ble_controller_enabled = true;

    ret = nimble_port_init();
    ESP_RETURN_ON_ERROR(ret, TAG, "nimble_port_init failed");
    taskENTER_CRITICAL(&s_state_lock);
    s_ble_host_ready = true;
    taskEXIT_CRITICAL(&s_state_lock);

    ble_hs_cfg.reset_cb = wireless_ble_on_reset;
    ble_hs_cfg.sync_cb = wireless_ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_ble_gatt_services);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "ble_gatts_count_cfg failed: %d", rc);
    rc = ble_gatts_add_svcs(s_ble_gatt_services);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "ble_gatts_add_svcs failed: %d", rc);
    rc = ble_svc_gap_device_name_set(WIRELESS_BLE_DEVICE_NAME);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "ble_svc_gap_device_name_set failed: %d", rc);

    ble_store_config_init();
    taskENTER_CRITICAL(&s_state_lock);
    s_ble_host_task_created = true;
    s_ble_host_task_started = false;
    taskEXIT_CRITICAL(&s_state_lock);
    nimble_port_freertos_init(wireless_ble_host_task);
    ESP_RETURN_ON_ERROR(wireless_wait_ble_host_started(), TAG, "BLE host task did not start");
    wireless_set_hint("ble init");
    return ESP_OK;
}

static esp_err_t wireless_wait_ble_host_stopped(void)
{
    TickType_t start = xTaskGetTickCount();
    while (true) {
        bool running;
        taskENTER_CRITICAL(&s_state_lock);
        running = s_ble_host_task_started;
        taskEXIT_CRITICAL(&s_state_lock);
        if (!running) {
            return ESP_OK;
        }
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(WIRELESS_BLE_STOP_TIMEOUT_MS)) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t wireless_deinit_ble(void)
{
    esp_err_t aggregate = ESP_OK;
    if (s_ble_host_ready) {
        (void)wireless_ble_stop_advertising();
        bool task_running;
        taskENTER_CRITICAL(&s_state_lock);
        task_running = s_ble_host_task_started;
        taskEXIT_CRITICAL(&s_state_lock);
        if (task_running) {
            int rc = nimble_port_stop();
            if (rc != 0) {
                ESP_LOGW(TAG, "nimble_port_stop failed: %d", rc);
                wireless_record_first_error(&aggregate, ESP_FAIL);
            }
        }
        esp_err_t wait_ret = wireless_wait_ble_host_stopped();
        wireless_record_first_error(&aggregate, wait_ret);
        if (wait_ret == ESP_OK) {
            if (s_ble_host_task_created) {
                nimble_port_freertos_deinit();
                s_ble_host_task_created = false;
            }
            esp_err_t ret = nimble_port_deinit();
            wireless_record_first_error(&aggregate, ret);
            if (ret == ESP_OK) {
                taskENTER_CRITICAL(&s_state_lock);
                s_ble_host_ready = false;
                s_ble_host_synced = false;
                taskEXIT_CRITICAL(&s_state_lock);
            }
        }
    }

    if (!s_ble_host_ready && s_ble_controller_enabled) {
        esp_err_t ret = esp_hosted_bt_controller_disable();
        wireless_record_first_error(&aggregate, ret);
        if (ret == ESP_OK) {
            s_ble_controller_enabled = false;
        }
    }
    if (!s_ble_host_ready && !s_ble_controller_enabled && s_ble_controller_ready) {
        esp_err_t ret = esp_hosted_bt_controller_deinit(false);
        wireless_record_first_error(&aggregate, ret);
        if (ret == ESP_OK) {
            s_ble_controller_ready = false;
        }
    }

    if (!s_ble_host_ready && !s_ble_controller_ready) {
        int64_t now_us = wireless_now_us();
        taskENTER_CRITICAL(&s_state_lock);
        s_ble_connected = false;
        s_ble_test_value = 0;
        s_ble_test_value_handle = 0;
        s_status.ble_advertising_enabled = false;
        s_status.ble_connected = false;
        s_status.ble_host_synced = false;
        s_status.ble_test_value = 0;
        wireless_commit_locked(now_us);
        taskEXIT_CRITICAL(&s_state_lock);
    }
    return aggregate;
}
#else
static esp_err_t wireless_prepare_ble(void)
{
    if (WIRELESS_BLE_FEATURE_ENABLED) {
        int64_t now_us = wireless_now_us();
        taskENTER_CRITICAL(&s_state_lock);
        s_status.ble_state = TEST_WIRELESS_BLE_NOT_AVAILABLE;
        s_status.last_error = ESP_ERR_NOT_SUPPORTED;
        wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), "ble cfg disabled");
        wireless_commit_locked(now_us);
        taskEXIT_CRITICAL(&s_state_lock);
        ESP_LOGW(TAG, "BLE requested but hosted NimBLE configuration is disabled");
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static esp_err_t wireless_deinit_ble(void)
{
    return ESP_OK;
}
#endif

static const char *wireless_hosted_event_to_hint(int32_t event_id)
{
    switch (event_id) {
    case ESP_HOSTED_EVENT_CP_INIT:
        return "cp init";
    case ESP_HOSTED_EVENT_TRANSPORT_FAILURE:
        return "transport fail";
    case ESP_HOSTED_EVENT_TRANSPORT_UP:
        return "transport up";
    case ESP_HOSTED_EVENT_TRANSPORT_DOWN:
        return "transport down";
    default:
        return "hosted event";
    }
}

static void wireless_hosted_event_handler(void *arg, esp_event_base_t event_base,
                                          int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != ESP_HOSTED_EVENT || !wireless_activity_enter(false)) {
        return;
    }

    EventGroupHandle_t event_group;
    taskENTER_CRITICAL(&s_state_lock);
    event_group = s_hosted_event_group;
    taskEXIT_CRITICAL(&s_state_lock);

    if (event_id == ESP_HOSTED_EVENT_TRANSPORT_UP && event_group != NULL) {
        xEventGroupSetBits(event_group, WIRELESS_HOSTED_EVENT_TRANSPORT_UP_BIT);
    } else if (event_id == ESP_HOSTED_EVENT_TRANSPORT_DOWN && event_group != NULL) {
        xEventGroupSetBits(event_group, WIRELESS_HOSTED_EVENT_DOWN_BIT);
        xEventGroupClearBits(event_group, WIRELESS_HOSTED_READY_BITS);
    } else if (event_id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE && event_group != NULL) {
        xEventGroupSetBits(event_group, WIRELESS_HOSTED_EVENT_FAILURE_BIT);
        xEventGroupClearBits(event_group, WIRELESS_HOSTED_READY_BITS);
    } else if (event_id == ESP_HOSTED_EVENT_CP_INIT && event_group != NULL) {
        xEventGroupSetBits(event_group, WIRELESS_HOSTED_EVENT_CP_INIT_BIT);
    }

    int64_t now_us = wireless_now_us();
    taskENTER_CRITICAL(&s_state_lock);
    if (event_id != ESP_HOSTED_EVENT_CP_HEARTBEAT && event_id != ESP_HOSTED_EVENT_MEM_MONITOR) {
        s_status.hosted_last_event = event_id;
    }
    if (event_id == ESP_HOSTED_EVENT_TRANSPORT_UP) {
        s_status.transport_up_received = true;
        s_status.transport_confirmed = true;
        s_status.transport_state = TEST_WIRELESS_TRANSPORT_READY;
        s_status.last_error = ESP_OK;
        if (s_status.initialized) {
            s_status.module.state = TEST_MODULE_STATE_RUNNING;
        }
    } else if (event_id == ESP_HOSTED_EVENT_TRANSPORT_DOWN ||
               event_id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE) {
        s_status.transport_confirmed = false;
        s_status.transport_state = TEST_WIRELESS_TRANSPORT_ERROR;
        if (s_status.wifi_feature_enabled) {
            s_wifi_connected_generation = 0;
            s_status.wifi_connected = false;
            s_status.wifi_rssi = 0;
            wireless_copy_string(s_status.current_ip, sizeof(s_status.current_ip), "--");
            s_status.wifi_state = TEST_WIRELESS_WIFI_ERROR;
        }
        s_status.last_error = ESP_FAIL;
        s_status.module.state = TEST_MODULE_STATE_ERROR;
    } else if (event_id == ESP_HOSTED_EVENT_CP_INIT) {
        s_status.cp_init_received = true;
    }
    if (event_id != ESP_HOSTED_EVENT_CP_HEARTBEAT && event_id != ESP_HOSTED_EVENT_MEM_MONITOR) {
        wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint),
                             wireless_hosted_event_to_hint(event_id));
        wireless_commit_locked(now_us);
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (event_id == ESP_HOSTED_EVENT_CP_HEARTBEAT || event_id == ESP_HOSTED_EVENT_MEM_MONITOR) {
        ESP_LOGD(TAG, "hosted event: %" PRIi32, event_id);
    } else {
        ESP_LOGI(TAG, "hosted event: %" PRIi32, event_id);
    }
    if (event_id == ESP_HOSTED_EVENT_CP_INIT && event_data != NULL) {
        const esp_hosted_event_init_t *init_event = (const esp_hosted_event_init_t *)event_data;
        ESP_LOGD(TAG, "co-processor reset reason: %d", init_event->reason);
    }
    wireless_activity_exit();
}

static uint32_t wireless_reconnect_delay_ms(uint32_t retry_index)
{
    uint32_t shift = retry_index > 4 ? 4 : retry_index;
    uint32_t delay_ms = WIRELESS_RECONNECT_BASE_DELAY_MS << shift;
    return delay_ms > WIRELESS_RECONNECT_MAX_DELAY_MS ? WIRELESS_RECONNECT_MAX_DELAY_MS : delay_ms;
}

static esp_err_t wireless_schedule_reconnect(uint32_t expected_generation)
{
    uint32_t attempt;
    uint32_t delay_ms;
    esp_err_t ret = ESP_OK;
    int64_t now_us = wireless_now_us();

    ESP_RETURN_ON_FALSE(s_wifi_command_mutex != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "WiFi command sync not ready");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_wifi_command_mutex, portMAX_DELAY) == pdTRUE,
                        ESP_ERR_INVALID_STATE, TAG, "lock WiFi command failed");

    taskENTER_CRITICAL(&s_state_lock);
    if (s_wifi_teardown || !s_wifi_started || !s_status.initialized ||
        !s_callbacks_enabled || !s_status.transport_confirmed ||
        !s_wifi_connect_requested || expected_generation == 0 ||
        expected_generation != s_wifi_request_generation ||
        s_status.wifi_retry_count >= WIRELESS_WIFI_MAXIMUM_RETRY) {
        taskEXIT_CRITICAL(&s_state_lock);
        xSemaphoreGive(s_wifi_command_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    attempt = ++s_status.wifi_retry_count;
    delay_ms = wireless_reconnect_delay_ms(attempt - 1);
    s_reconnect_timer_generation = expected_generation;
    s_status.wifi_state = TEST_WIRELESS_WIFI_CONNECTING;
    s_status.last_error = ESP_OK;
    wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), "wifi retry wait");
    wireless_commit_locked(now_us);
    taskEXIT_CRITICAL(&s_state_lock);

    if (s_reconnect_timer == NULL ||
        xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(delay_ms), 0) != pdPASS) {
        s_reconnect_timer_generation = 0;
        ret = ESP_FAIL;
        taskENTER_CRITICAL(&s_state_lock);
        if (expected_generation == s_wifi_request_generation) {
            s_status.last_error = ret;
            wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint),
                                 "wifi retry timer fail");
            wireless_commit_locked(now_us);
        }
        taskEXIT_CRITICAL(&s_state_lock);
    }
    xSemaphoreGive(s_wifi_command_mutex);

    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "WiFi reconnect attempt %" PRIu32 "/%" PRIu32 " in %" PRIu32 " ms",
             attempt, (uint32_t)WIRELESS_WIFI_MAXIMUM_RETRY, delay_ms);
    return ESP_OK;
}

static void wireless_reconnect_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    if (!wireless_activity_enter(false)) {
        return;
    }

    bool reconnect_allowed = false;
    uint32_t generation = 0;
    if (s_wifi_command_mutex != NULL &&
        xSemaphoreTake(s_wifi_command_mutex, portMAX_DELAY) == pdTRUE) {
        generation = s_reconnect_timer_generation;
        s_reconnect_timer_generation = 0;
        xSemaphoreGive(s_wifi_command_mutex);
    }

    esp_err_t claim_ret = wireless_wifi_command_claim(true);
    if (claim_ret == ESP_OK) {
        reconnect_allowed = wireless_wifi_reconnect_allowed_locked(generation);
    }

    esp_err_t connect_ret = ESP_OK;
    if (reconnect_allowed) {
        connect_ret = esp_wifi_connect();
        if (connect_ret != ESP_OK) {
            wireless_set_error(connect_ret, "wifi reconnect call fail");
        } else {
            wireless_set_hint("wifi retry sent");
        }
    }
    if (claim_ret == ESP_OK) {
        wireless_wifi_command_release();
    }
    if (reconnect_allowed && connect_ret != ESP_OK &&
        wireless_schedule_reconnect(generation) != ESP_OK) {
        int64_t now_us = wireless_now_us();
        if (xSemaphoreTake(s_wifi_command_mutex, portMAX_DELAY) == pdTRUE) {
            if (wireless_wifi_reconnect_allowed_locked(generation)) {
                taskENTER_CRITICAL(&s_state_lock);
                s_status.wifi_state = TEST_WIRELESS_WIFI_ERROR;
                s_status.last_error = connect_ret;
                wireless_commit_locked(now_us);
                taskEXIT_CRITICAL(&s_state_lock);
            }
            xSemaphoreGive(s_wifi_command_mutex);
        }
    }
    wireless_activity_exit();
}

static void wireless_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                        int32_t event_id, void *event_data)
{
    (void)arg;
    if (!wireless_activity_enter(false)) {
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    bool transport_confirmed = s_status.transport_confirmed;
    taskEXIT_CRITICAL(&s_state_lock);
    if (!transport_confirmed) {
        wireless_activity_exit();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        int64_t now_us = wireless_now_us();
        taskENTER_CRITICAL(&s_state_lock);
        if (s_status.transport_confirmed &&
            s_status.wifi_state != TEST_WIRELESS_WIFI_SCANNING &&
            s_status.wifi_state != TEST_WIRELESS_WIFI_CONNECTING &&
            s_status.wifi_state != TEST_WIRELESS_WIFI_CONNECTED) {
            s_status.wifi_state = TEST_WIRELESS_WIFI_IDLE;
            s_status.last_error = ESP_OK;
            wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), "wifi sta start");
            wireless_commit_locked(now_us);
        }
        taskEXIT_CRITICAL(&s_state_lock);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        const wifi_event_sta_scan_done_t *done = (const wifi_event_sta_scan_done_t *)event_data;
        ESP_LOGD(TAG, "scan done status=%" PRIu32 " number=%" PRIu8 " scan_id=%" PRIu8,
                 done != NULL ? done->status : 0, done != NULL ? done->number : 0,
                 done != NULL ? done->scan_id : 0);
        esp_err_t ret;
        if (done != NULL && done->status != 0U) {
            ret = ESP_FAIL;
            wireless_publish_scan(NULL, 0, 0, false, ret, "scan done fail");
        } else {
            ret = wireless_update_scan_results();
        }
        int64_t now_us = wireless_now_us();
        taskENTER_CRITICAL(&s_state_lock);
        if (s_status.transport_confirmed) {
            if (s_status.wifi_connected) {
                s_status.wifi_state = TEST_WIRELESS_WIFI_CONNECTED;
            } else if (s_status.wifi_state == TEST_WIRELESS_WIFI_SCANNING) {
                s_status.wifi_state = ret == ESP_OK ? TEST_WIRELESS_WIFI_IDLE :
                                                      TEST_WIRELESS_WIFI_ERROR;
            }
            wireless_commit_locked(now_us);
        }
        taskEXIT_CRITICAL(&s_state_lock);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *connected = (const wifi_event_sta_connected_t *)event_data;
        char ssid[TEST_WIRELESS_SSID_MAX_LEN + 1] = {0};
        if (connected != NULL) {
            snprintf(ssid, sizeof(ssid), "%.*s", (int)connected->ssid_len, connected->ssid);
        }
        if (s_wifi_command_mutex != NULL &&
            xSemaphoreTake(s_wifi_command_mutex, portMAX_DELAY) == pdTRUE) {
            int64_t now_us = wireless_now_us();
            taskENTER_CRITICAL(&s_state_lock);
            bool accept = !s_wifi_teardown && s_status.initialized &&
                          s_callbacks_enabled && s_status.transport_confirmed &&
                          s_wifi_connect_requested &&
                          connected != NULL &&
                          wireless_wifi_ssid_matches_locked(connected->ssid,
                                                            connected->ssid_len);
            if (accept) {
                s_suppress_next_disconnect = false;
                s_wifi_connected_generation = s_wifi_request_generation;
                s_status.wifi_connected = true;
                s_status.wifi_retry_count = 0;
                s_status.wifi_rssi = 0;
                wireless_copy_string(s_status.current_ssid, sizeof(s_status.current_ssid), ssid);
                wireless_copy_string(s_status.current_ip, sizeof(s_status.current_ip), "--");
                s_status.wifi_state = TEST_WIRELESS_WIFI_CONNECTED;
                s_status.last_error = ESP_OK;
                wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), "wifi connected");
                wireless_commit_locked(now_us);
            }
            taskEXIT_CRITICAL(&s_state_lock);
            if (accept) {
                wireless_stop_reconnect_timer_locked();
            }
            xSemaphoreGive(s_wifi_command_mutex);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected = (const wifi_event_sta_disconnected_t *)event_data;
        bool requested = false;
        bool suppress = false;
        bool stale = false;
        uint32_t retry_count = 0;
        uint32_t request_generation = 0;
        int64_t now_us = wireless_now_us();
        if (s_wifi_command_mutex != NULL &&
            xSemaphoreTake(s_wifi_command_mutex, portMAX_DELAY) == pdTRUE) {
            taskENTER_CRITICAL(&s_state_lock);
            if (!s_status.transport_confirmed) {
                stale = true;
            } else {
                requested = !s_wifi_teardown && s_status.initialized &&
                            s_callbacks_enabled && s_wifi_connect_requested;
                suppress = s_suppress_next_disconnect;
                retry_count = s_status.wifi_retry_count;
                request_generation = s_wifi_request_generation;
                stale = requested && !suppress && disconnected != NULL &&
                        !wireless_wifi_ssid_matches_locked(disconnected->ssid,
                                                           disconnected->ssid_len);
            }
            if (!stale) {
                s_suppress_next_disconnect = false;
                s_wifi_connected_generation = 0;
                s_status.wifi_connected = false;
                s_status.wifi_rssi = disconnected != NULL ? disconnected->rssi : 0;
                wireless_copy_string(s_status.current_ip, sizeof(s_status.current_ip), "--");
                if (suppress && requested) {
                    s_status.wifi_state = TEST_WIRELESS_WIFI_CONNECTING;
                    wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), "wifi replace link");
                } else if (!requested) {
                    s_status.wifi_state = TEST_WIRELESS_WIFI_IDLE;
                    s_status.last_error = ESP_OK;
                    wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), "wifi idle");
                }
                wireless_commit_locked(now_us);
            }
            taskEXIT_CRITICAL(&s_state_lock);
            xSemaphoreGive(s_wifi_command_mutex);
        }

        if (!stale && !suppress && requested) {
            if (retry_count < WIRELESS_WIFI_MAXIMUM_RETRY &&
                wireless_schedule_reconnect(request_generation) == ESP_OK) {
                /* Reconnect is handled by the one-shot timer. */
            } else {
                const char *hint = disconnected != NULL && wireless_reason_is_no_ap_found(disconnected->reason) ?
                                   "no ap found" : "wifi disconnect";
                now_us = wireless_now_us();
                if (xSemaphoreTake(s_wifi_command_mutex, portMAX_DELAY) == pdTRUE) {
                    if (wireless_wifi_reconnect_allowed_locked(request_generation)) {
                        taskENTER_CRITICAL(&s_state_lock);
                        s_status.wifi_state = TEST_WIRELESS_WIFI_ERROR;
                        s_status.last_error = ESP_FAIL;
                        wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), hint);
                        wireless_commit_locked(now_us);
                        taskEXIT_CRITICAL(&s_state_lock);
                    }
                    xSemaphoreGive(s_wifi_command_mutex);
                }
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got_ip = (const ip_event_got_ip_t *)event_data;
        char ip[TEST_WIRELESS_IPV4_STR_LEN] = "--";
        if (got_ip != NULL) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&got_ip->ip_info.ip));
        }
        wifi_ap_record_t ap_info = {0};
        esp_err_t rssi_ret = esp_wifi_sta_get_ap_info(&ap_info);
        if (s_wifi_command_mutex != NULL &&
            xSemaphoreTake(s_wifi_command_mutex, portMAX_DELAY) == pdTRUE) {
            int64_t now_us = wireless_now_us();
            taskENTER_CRITICAL(&s_state_lock);
            const size_t ap_ssid_len = rssi_ret == ESP_OK ?
                                       strnlen((const char *)ap_info.ssid,
                                               sizeof(ap_info.ssid)) : 0;
            const bool ap_matches = rssi_ret != ESP_OK ||
                                    wireless_wifi_ssid_matches_locked(ap_info.ssid,
                                                                      ap_ssid_len);
            bool accept = !s_wifi_teardown && s_status.initialized &&
                          s_callbacks_enabled && s_status.transport_confirmed &&
                          s_wifi_connect_requested &&
                          s_wifi_connected_generation != 0 &&
                          s_wifi_connected_generation == s_wifi_request_generation &&
                          ap_matches;
            if (accept) {
                s_suppress_next_disconnect = false;
                s_status.wifi_connected = true;
                s_status.wifi_state = TEST_WIRELESS_WIFI_CONNECTED;
                s_status.wifi_retry_count = 0;
                s_status.wifi_rssi = rssi_ret == ESP_OK ? ap_info.rssi : s_status.wifi_rssi;
                s_status.last_error = ESP_OK;
                wireless_copy_string(s_status.current_ip, sizeof(s_status.current_ip), ip);
                wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), "ip got");
                wireless_commit_locked(now_us);
            }
            taskEXIT_CRITICAL(&s_state_lock);
            if (accept) {
                wireless_stop_reconnect_timer_locked();
            }
            xSemaphoreGive(s_wifi_command_mutex);
        }
    }

    wireless_activity_exit();
}

static esp_err_t wireless_wait_hosted_ready(void)
{
    EventGroupHandle_t event_group;
    taskENTER_CRITICAL(&s_state_lock);
    event_group = s_hosted_event_group;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_RETURN_ON_FALSE(event_group != NULL, ESP_ERR_INVALID_STATE, TAG, "hosted event group not ready");

    EventBits_t bits = xEventGroupWaitBits(event_group,
                                           WIRELESS_HOSTED_EVENT_TRANSPORT_UP_BIT |
                                           WIRELESS_HOSTED_EVENT_FAILURE_BIT |
                                           WIRELESS_HOSTED_EVENT_DOWN_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIRELESS_HOSTED_WAIT_READY_MS));
    if ((bits & (WIRELESS_HOSTED_EVENT_FAILURE_BIT | WIRELESS_HOSTED_EVENT_DOWN_BIT)) != 0U) {
        wireless_set_transport_error(ESP_FAIL, "transport failure evt");
        return ESP_FAIL;
    }
    if ((bits & WIRELESS_HOSTED_EVENT_TRANSPORT_UP_BIT) == 0U) {
        wireless_set_transport_error(ESP_ERR_TIMEOUT, "transport wait timeout");
        return ESP_ERR_TIMEOUT;
    }

    /* The process-lifetime Hosted transport does not emit TRANSPORT_UP again
     * when a later module generation reuses it. Also close the wake-up race
     * where the event bit becomes visible before the event handler publishes
     * the matching status snapshot. */
    int64_t now_us = wireless_now_us();
    esp_err_t ret = ESP_OK;
    taskENTER_CRITICAL(&s_state_lock);
    if (s_status.transport_state == TEST_WIRELESS_TRANSPORT_ERROR) {
        ret = s_status.last_error != ESP_OK ? s_status.last_error : ESP_FAIL;
    } else {
        s_status.transport_up_received = true;
        s_status.transport_confirmed = true;
        s_status.transport_state = TEST_WIRELESS_TRANSPORT_READY;
        s_status.last_error = ESP_OK;
        wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), "transport up");
        wireless_commit_locked(now_us);
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return ret;
}

static esp_err_t wireless_prepare_stack(void)
{
    if (WIRELESS_WIFI_FEATURE_ENABLED && !s_netif_ready) {
        ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
        s_netif_ready = true;
    }
    if (!s_event_loop_ready) {
        esp_err_t ret = esp_event_loop_create_default();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            return ret;
        }
        s_event_loop_ready = true;
    }
    if (WIRELESS_WIFI_FEATURE_ENABLED && s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_ERR_NO_MEM, TAG, "create wifi sta netif failed");
    }
    if (WIRELESS_WIFI_FEATURE_ENABLED && s_reconnect_timer == NULL) {
        s_reconnect_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(WIRELESS_RECONNECT_BASE_DELAY_MS),
                                        pdFALSE, NULL, wireless_reconnect_timer_callback);
    }
    ESP_RETURN_ON_FALSE(!WIRELESS_WIFI_FEATURE_ENABLED || s_reconnect_timer != NULL,
                        ESP_ERR_NO_MEM, TAG, "create reconnect timer failed");
    return ESP_OK;
}

static esp_err_t wireless_prepare_hosted(void)
{
    s_hosted_event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_hosted_event_group != NULL, ESP_ERR_NO_MEM, TAG, "create hosted event group failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID,
                                                            wireless_hosted_event_handler, NULL,
                                                            &s_hosted_evt_instance),
                        TAG, "register hosted event failed");
    s_hosted_handler_registered = true;
    s_hosted_ready = true;

    int64_t now_us = wireless_now_us();
    taskENTER_CRITICAL(&s_state_lock);
    s_callbacks_enabled = true;
    s_status.transport_state = TEST_WIRELESS_TRANSPORT_CONNECTING;
    s_status.transport_confirmed = false;
    s_status.rpc_metadata_ready = false;
    s_status.transport_up_received = false;
    s_status.cp_init_received = false;
    s_status.hosted_cp_info_error = ESP_OK;
    s_status.hosted_fw_version_error = ESP_OK;
    s_status.last_error = ESP_OK;
    wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), "hosted connect");
    wireless_commit_locked(now_us);
    taskEXIT_CRITICAL(&s_state_lock);
    xEventGroupClearBits(s_hosted_event_group, WIRELESS_HOSTED_READY_BITS |
                        WIRELESS_HOSTED_EVENT_FAILURE_BIT | WIRELESS_HOSTED_EVENT_DOWN_BIT);

    ESP_RETURN_ON_ERROR(esp_hosted_connect_to_slave(), TAG, "esp_hosted_connect_to_slave failed");
    if (is_transport_tx_ready()) {
        xEventGroupSetBits(s_hosted_event_group, WIRELESS_HOSTED_EVENT_TRANSPORT_UP_BIT);
    }
    ESP_RETURN_ON_ERROR(wireless_wait_hosted_ready(), TAG, "hosted transport wait failed");
    wireless_record_cp_info();
    return ESP_OK;
}

static esp_err_t wireless_prepare_wifi(void)
{
    if (!WIRELESS_WIFI_FEATURE_ENABLED) {
        return ESP_OK;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret == ESP_OK || ret == ESP_ERR_WIFI_INIT_STATE) {
        /* This module is the application's sole WiFi owner. An initialized
         * driver is treated as residue from an earlier module lifetime. */
        s_wifi_driver_owned = true;
    } else {
        ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_init failed");
    }
    s_wifi_driver_ready = true;

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            wireless_wifi_event_handler, NULL,
                                                            &s_wifi_evt_instance),
                        TAG, "register wifi event failed");
    s_wifi_event_registered = true;
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            wireless_wifi_event_handler, NULL,
                                                            &s_ip_evt_instance),
                        TAG, "register ip event failed");
    s_ip_event_registered = true;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
    s_wifi_started = true;
    return ESP_OK;
}

static esp_err_t wireless_unregister_handlers(void)
{
    esp_err_t aggregate = ESP_OK;
    if (s_ip_event_registered) {
        esp_err_t ret = esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_evt_instance);
        wireless_record_first_error(&aggregate, ret);
        if (ret == ESP_OK) {
            s_ip_event_registered = false;
        }
    }
    if (s_wifi_event_registered) {
        esp_err_t ret = esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_evt_instance);
        wireless_record_first_error(&aggregate, ret);
        if (ret == ESP_OK) {
            s_wifi_event_registered = false;
        }
    }
    if (s_hosted_handler_registered) {
        esp_err_t ret = esp_event_handler_instance_unregister(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID,
                                                               s_hosted_evt_instance);
        wireless_record_first_error(&aggregate, ret);
        if (ret == ESP_OK) {
            s_hosted_handler_registered = false;
        }
    }
    return aggregate;
}

static esp_err_t wireless_release_resources(void)
{
    esp_err_t aggregate = ESP_OK;
    if (s_reconnect_timer != NULL && xTimerStop(s_reconnect_timer, pdMS_TO_TICKS(100)) != pdPASS) {
        wireless_record_first_error(&aggregate, ESP_FAIL);
    }
    esp_err_t unregister_ret = wireless_unregister_handlers();
    wireless_record_first_error(&aggregate, unregister_ret);
    if (unregister_ret != ESP_OK) {
        return aggregate;
    }
    esp_err_t drain_ret = wireless_wait_activity_idle(WIRELESS_CALLBACK_DRAIN_TIMEOUT_MS);
    wireless_record_first_error(&aggregate, drain_ret);
    if (drain_ret != ESP_OK) {
        return aggregate;
    }

    wireless_record_first_error(&aggregate, wireless_deinit_ble());

    if (s_wifi_started) {
        esp_err_t ret = esp_wifi_stop();
        wireless_record_first_error(&aggregate, ret);
        if (ret == ESP_OK) {
            s_wifi_started = false;
        }
    }
    if (!s_wifi_started && s_wifi_driver_ready && s_wifi_driver_owned) {
        esp_err_t ret = esp_wifi_deinit();
        wireless_record_first_error(&aggregate, ret);
        if (ret == ESP_OK) {
            s_wifi_driver_ready = false;
            s_wifi_driver_owned = false;
        }
    } else if (!s_wifi_driver_owned) {
        s_wifi_driver_ready = false;
    }

    if (s_reconnect_timer != NULL) {
        if (xTimerDelete(s_reconnect_timer, pdMS_TO_TICKS(100)) == pdPASS) {
            s_reconnect_timer = NULL;
        } else {
            wireless_record_first_error(&aggregate, ESP_FAIL);
        }
    }
    if (s_hosted_event_group != NULL) {
        vEventGroupDelete(s_hosted_event_group);
        taskENTER_CRITICAL(&s_state_lock);
        s_hosted_event_group = NULL;
        taskEXIT_CRITICAL(&s_state_lock);
    }
    if (s_sta_netif != NULL && !s_wifi_started && !s_wifi_driver_ready) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    /* The default event loop and esp_netif are process-wide services. esp_hosted
     * outlives this module, so their lifetime intentionally remains global. */
    s_hosted_ready = false;
    return aggregate;
}

static bool wireless_has_owned_resources(void)
{
    return s_hosted_handler_registered || s_wifi_event_registered || s_ip_event_registered ||
           s_wifi_started || (s_wifi_driver_ready && s_wifi_driver_owned) ||
           s_ble_host_ready || s_ble_controller_ready || s_reconnect_timer != NULL ||
           s_hosted_event_group != NULL || s_sta_netif != NULL;
}

static void wireless_initialize_status(void)
{
    test_wireless_status_t initial = {
        .module = {
            .state = TEST_MODULE_STATE_INITIALIZING,
            .last_error = ESP_OK,
            .generation = s_status_generation,
        },
        .init_attempted = true,
        .hosted_stack_available = true,
        .wifi_feature_enabled = WIRELESS_WIFI_FEATURE_ENABLED,
        .ble_feature_enabled = WIRELESS_BLE_FEATURE_ENABLED,
        .sdio_board_link_defined = true,
        .transport_state = TEST_WIRELESS_TRANSPORT_IDLE,
        .wifi_state = WIRELESS_WIFI_FEATURE_ENABLED ? TEST_WIRELESS_WIFI_IDLE :
                                                     TEST_WIRELESS_WIFI_NOT_AVAILABLE,
        .ble_state = WIRELESS_BLE_FEATURE_ENABLED ?
                     (WIRELESS_BLE_HOSTED_ENABLED ? TEST_WIRELESS_BLE_IDLE : TEST_WIRELESS_BLE_NOT_AVAILABLE) :
                     TEST_WIRELESS_BLE_UNINITIALIZED,
        .hosted_last_event = -1,
        .last_error = ESP_OK,
    };
    wireless_copy_string(initial.current_ip, sizeof(initial.current_ip), "--");
    wireless_copy_string(initial.cp_target_name, sizeof(initial.cp_target_name), "--");
    wireless_copy_string(initial.cp_fw_version, sizeof(initial.cp_fw_version), "--");
    wireless_copy_string(initial.ble_device_name, sizeof(initial.ble_device_name), WIRELESS_BLE_DEVICE_NAME);
    wireless_copy_string(initial.last_hint, sizeof(initial.last_hint), "wifi hosted init");

    int64_t now_us = wireless_now_us();
    (void)xSemaphoreTake(s_wifi_command_mutex, portMAX_DELAY);
    taskENTER_CRITICAL(&s_state_lock);
    s_status = initial;
    s_actions_enabled = false;
    s_callbacks_enabled = false;
    s_wifi_connect_requested = false;
    s_suppress_next_disconnect = false;
    s_wifi_connected_generation = 0;
    s_wifi_teardown = false;
    s_reconnect_timer_generation = 0;
    (void)wireless_next_wifi_request_generation_locked();
    wireless_commit_locked(now_us);
    taskEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(s_wifi_command_mutex);
    wireless_publish_scan(NULL, 0, 0, false, ESP_OK, "wifi hosted init");
}

esp_err_t test_wireless_init(void)
{
    int64_t claim_time_us = wireless_now_us();
    taskENTER_CRITICAL(&s_state_lock);
    if (s_status.initialized) {
        bool failed = s_status.module.state == TEST_MODULE_STATE_ERROR ||
                      s_status.transport_state == TEST_WIRELESS_TRANSPORT_ERROR ||
                      !s_status.transport_confirmed;
        esp_err_t error = s_status.last_error != ESP_OK ? s_status.last_error :
                                                           ESP_ERR_INVALID_STATE;
        taskEXIT_CRITICAL(&s_state_lock);
        return failed ? error : ESP_OK;
    }
    if (s_status.module.state == TEST_MODULE_STATE_INITIALIZING ||
        s_status.module.state == TEST_MODULE_STATE_STOPPING) {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_status.module.state = TEST_MODULE_STATE_INITIALIZING;
    s_actions_enabled = false;
    s_callbacks_enabled = false;
    wireless_commit_locked(claim_time_us);
    taskEXIT_CRITICAL(&s_state_lock);

    esp_err_t ret = wireless_prepare_wifi_command_sync();
    if (ret != ESP_OK) {
        wireless_set_transport_error(ret, "wifi command sync fail");
        return ret;
    }

    if (wireless_has_owned_resources()) {
        taskENTER_CRITICAL(&s_state_lock);
        s_actions_enabled = false;
        s_callbacks_enabled = false;
        taskEXIT_CRITICAL(&s_state_lock);
        esp_err_t cleanup_ret = wireless_release_resources();
        if (cleanup_ret != ESP_OK || wireless_has_owned_resources()) {
            esp_err_t error = cleanup_ret != ESP_OK ? cleanup_ret : ESP_ERR_INVALID_STATE;
            wireless_set_transport_error(error, "previous cleanup incomplete");
            return error;
        }
    }

    wireless_initialize_status();
    ret = wireless_validate_hosted_config();
    if (ret == ESP_OK) {
        ret = wireless_prepare_stack();
    }
    if (ret == ESP_OK) {
        ret = wireless_prepare_hosted();
    }
    if (ret == ESP_OK) {
        ret = wireless_prepare_wifi();
    }
    if (ret == ESP_OK) {
        ret = wireless_prepare_ble();
    }

    if (ret != ESP_OK) {
        goto fail;
    }

    int64_t now_us = wireless_now_us();
    test_wireless_status_t status = {0};
    taskENTER_CRITICAL(&s_state_lock);
    if (!s_status.transport_confirmed ||
        s_status.transport_state != TEST_WIRELESS_TRANSPORT_READY) {
        ret = s_status.last_error != ESP_OK ? s_status.last_error : ESP_FAIL;
    } else {
        s_status.initialized = true;
        s_status.module.state = TEST_MODULE_STATE_READY;
        s_status.last_error = ESP_OK;
        s_actions_enabled = true;
        wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint),
                             WIRELESS_BLE_FEATURE_ENABLED ? "wireless ready" : "wifi ready");
        wireless_advance_lifecycle_locked();
        wireless_commit_locked(now_us);
        status = s_status;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (ret != ESP_OK) {
        goto fail;
    }

    ESP_LOGI(TAG, "WiFi hosted path verified on P4<->C6 SDIO");
    ESP_LOGI(TAG, "co-processor target=%s chip=0x%" PRIx32 " fw=%s rpc_meta=%d",
             status.cp_target_name, status.cp_chip_id, status.cp_fw_version,
             status.rpc_metadata_ready);
    return ESP_OK;

fail:
    taskENTER_CRITICAL(&s_state_lock);
    s_actions_enabled = false;
    s_callbacks_enabled = false;
    taskEXIT_CRITICAL(&s_state_lock);
    esp_err_t cleanup_ret = wireless_release_resources();
    if (cleanup_ret != ESP_OK) {
        ESP_LOGE(TAG, "Wireless init rollback failed: %s", esp_err_to_name(cleanup_ret));
    }
    wireless_set_transport_error(ret, "wireless init fail");
    ESP_LOGE(TAG, "Wireless init failed: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t test_wireless_report_dependency_failure(esp_err_t error)
{
    ESP_RETURN_ON_FALSE(error != ESP_OK, ESP_ERR_INVALID_ARG, TAG,
                        "dependency error must not be ESP_OK");

    test_wireless_status_t failure = {
        .module = {
            .state = TEST_MODULE_STATE_ERROR,
            .last_error = error,
        },
        .init_attempted = true,
        .hosted_stack_available = true,
        .wifi_feature_enabled = WIRELESS_WIFI_FEATURE_ENABLED,
        .ble_feature_enabled = WIRELESS_BLE_FEATURE_ENABLED,
        .sdio_board_link_defined = true,
        .transport_state = TEST_WIRELESS_TRANSPORT_UNINITIALIZED,
        .wifi_state = WIRELESS_WIFI_FEATURE_ENABLED ? TEST_WIRELESS_WIFI_ERROR :
                                                     TEST_WIRELESS_WIFI_NOT_AVAILABLE,
        .ble_state = WIRELESS_BLE_FEATURE_ENABLED ? TEST_WIRELESS_BLE_ERROR :
                                                   TEST_WIRELESS_BLE_NOT_AVAILABLE,
        .hosted_last_event = -1,
        .last_error = error,
    };
    wireless_copy_string(failure.current_ip, sizeof(failure.current_ip), "--");
    wireless_copy_string(failure.cp_target_name, sizeof(failure.cp_target_name), "--");
    wireless_copy_string(failure.cp_fw_version, sizeof(failure.cp_fw_version), "--");
    wireless_copy_string(failure.ble_device_name, sizeof(failure.ble_device_name),
                         WIRELESS_BLE_DEVICE_NAME);
    wireless_copy_string(failure.last_hint, sizeof(failure.last_hint), "dependency failure");

    int64_t now_us = wireless_now_us();
    taskENTER_CRITICAL(&s_state_lock);
    if (s_status.initialized || s_status.module.state == TEST_MODULE_STATE_INITIALIZING ||
        s_status.module.state == TEST_MODULE_STATE_STOPPING) {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    failure.module.generation = s_status_generation;
    s_status = failure;
    wireless_commit_locked(now_us);
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t test_wireless_run(void)
{
    ESP_RETURN_ON_ERROR(wireless_require_initialized(), TAG, "wireless init state invalid");
    ESP_RETURN_ON_FALSE(wireless_activity_enter(true), ESP_ERR_INVALID_STATE, TAG, "wireless is stopping");
    int64_t now_us = wireless_now_us();
    esp_err_t ret = ESP_OK;
    taskENTER_CRITICAL(&s_state_lock);
    if (!s_status.initialized || !s_actions_enabled ||
        s_status.module.state == TEST_MODULE_STATE_ERROR ||
        s_status.transport_state == TEST_WIRELESS_TRANSPORT_ERROR ||
        !s_status.transport_confirmed) {
        ret = s_status.last_error != ESP_OK ? s_status.last_error : ESP_ERR_INVALID_STATE;
    } else {
        s_status.module.state = TEST_MODULE_STATE_RUNNING;
        s_status.last_error = ESP_OK;
        wireless_commit_locked(now_us);
    }
    taskEXIT_CRITICAL(&s_state_lock);
    wireless_activity_exit();
    return ret;
}

esp_err_t test_wireless_get_status(test_wireless_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != NULL, ESP_ERR_INVALID_ARG, TAG, "out_status is NULL");
    taskENTER_CRITICAL(&s_state_lock);
    *out_status = s_status;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t test_wireless_get_scan_snapshot(test_wireless_scan_snapshot_t *out_snapshot)
{
    ESP_RETURN_ON_FALSE(out_snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "out_snapshot is NULL");
    taskENTER_CRITICAL(&s_state_lock);
    *out_snapshot = s_scan_snapshot;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t test_wireless_get_scan_results(test_wireless_ap_record_t *records, size_t max_records,
                                         size_t *out_count)
{
    ESP_RETURN_ON_ERROR(wireless_require_initialized(), TAG, "wireless init state invalid");
    ESP_RETURN_ON_FALSE(out_count != NULL, ESP_ERR_INVALID_ARG, TAG, "out_count is NULL");

    test_wireless_scan_snapshot_t snapshot;
    ESP_RETURN_ON_ERROR(test_wireless_get_scan_snapshot(&snapshot), TAG, "scan snapshot failed");
    size_t copy_count = snapshot.record_count > max_records ? max_records : snapshot.record_count;
    if (copy_count > 0) {
        ESP_RETURN_ON_FALSE(records != NULL, ESP_ERR_INVALID_ARG, TAG, "records is NULL");
        memcpy(records, snapshot.records, copy_count * sizeof(*records));
    }
    *out_count = copy_count;
    return ESP_OK;
}

esp_err_t test_wireless_request_wifi_scan(void)
{
    ESP_RETURN_ON_ERROR(wireless_require_initialized(), TAG, "wireless init state invalid");
    ESP_RETURN_ON_FALSE(WIRELESS_WIFI_FEATURE_ENABLED, ESP_ERR_NOT_SUPPORTED, TAG, "WiFi feature disabled");
    ESP_RETURN_ON_ERROR(wireless_require_transport_confirmed(), TAG, "transport not confirmed");
    ESP_RETURN_ON_FALSE(wireless_activity_enter(true), ESP_ERR_INVALID_STATE, TAG, "wireless is stopping");

    esp_err_t ret = wireless_wifi_command_claim(false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi command busy, scan rejected");
        wireless_activity_exit();
        return ret;
    }
    if (!wireless_wifi_action_allowed_locked(true)) {
        wireless_wifi_command_release();
        wireless_activity_exit();
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_state_lock);
    bool scan_busy = s_status.wifi_state == TEST_WIRELESS_WIFI_SCANNING ||
                     s_status.wifi_state == TEST_WIRELESS_WIFI_CONNECTING;
    taskEXIT_CRITICAL(&s_state_lock);
    if (scan_busy) {
        wireless_wifi_command_release();
        wireless_activity_exit();
        return ESP_ERR_INVALID_STATE;
    }

    wireless_publish_scan(NULL, 0, 0, false, ESP_OK, "wifi scan start");
    int64_t now_us = wireless_now_us();
    taskENTER_CRITICAL(&s_state_lock);
    s_status.wifi_state = TEST_WIRELESS_WIFI_SCANNING;
    wireless_commit_locked(now_us);
    taskEXIT_CRITICAL(&s_state_lock);
    ret = esp_wifi_scan_start(NULL, false);
    if (ret != ESP_OK) {
        now_us = wireless_now_us();
        taskENTER_CRITICAL(&s_state_lock);
        s_status.wifi_state = s_status.wifi_connected ? TEST_WIRELESS_WIFI_CONNECTED :
                                                        TEST_WIRELESS_WIFI_ERROR;
        s_status.last_error = ret;
        wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), "scan start fail");
        wireless_commit_locked(now_us);
        taskEXIT_CRITICAL(&s_state_lock);
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(ret));
    }
    wireless_wifi_command_release();
    wireless_activity_exit();
    return ret;
}

esp_err_t test_wireless_request_wifi_connect(const char *ssid, const char *password)
{
    ESP_RETURN_ON_ERROR(wireless_require_initialized(), TAG, "wireless init state invalid");
    ESP_RETURN_ON_FALSE(WIRELESS_WIFI_FEATURE_ENABLED, ESP_ERR_NOT_SUPPORTED, TAG, "WiFi feature disabled");
    ESP_RETURN_ON_ERROR(wireless_require_transport_confirmed(), TAG, "transport not confirmed");
    ESP_RETURN_ON_FALSE(ssid != NULL, ESP_ERR_INVALID_ARG, TAG, "ssid is NULL");
    size_t ssid_len = strnlen(ssid, TEST_WIRELESS_SSID_MAX_LEN + 1);
    ESP_RETURN_ON_FALSE(ssid_len > 0 && ssid_len <= TEST_WIRELESS_SSID_MAX_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "ssid length must be 1..%d",
                        TEST_WIRELESS_SSID_MAX_LEN);
    size_t password_len = password != NULL ?
                          strnlen(password, TEST_WIRELESS_PASSWORD_MAX_LEN + 1) : 0;
    ESP_RETURN_ON_FALSE(password_len <= TEST_WIRELESS_PASSWORD_MAX_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "password length exceeds %d",
                        TEST_WIRELESS_PASSWORD_MAX_LEN);
    ESP_RETURN_ON_FALSE(wireless_activity_enter(true), ESP_ERR_INVALID_STATE, TAG, "wireless is stopping");

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = wireless_auth_mode_threshold(),
        },
    };
    memcpy(wifi_config.sta.ssid, ssid, ssid_len);
    if (password_len > 0) {
        memcpy(wifi_config.sta.password, password, password_len);
    }

    esp_err_t ret = wireless_wifi_command_claim(false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi command busy, connect rejected");
        wireless_activity_exit();
        return ret;
    }
    if (!wireless_wifi_action_allowed_locked(true)) {
        wireless_wifi_command_release();
        wireless_activity_exit();
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_state_lock);
    bool connect_busy = s_status.wifi_state == TEST_WIRELESS_WIFI_SCANNING ||
                        s_status.wifi_state == TEST_WIRELESS_WIFI_CONNECTING;
    taskEXIT_CRITICAL(&s_state_lock);
    if (connect_busy) {
        wireless_wifi_command_release();
        wireless_activity_exit();
        return ESP_ERR_INVALID_STATE;
    }

    (void)xSemaphoreTake(s_wifi_command_mutex, portMAX_DELAY);
    (void)wireless_next_wifi_request_generation_locked();
    wireless_stop_reconnect_timer_locked();
    taskENTER_CRITICAL(&s_state_lock);
    s_wifi_connect_requested = false;
    s_suppress_next_disconnect = true;
    s_wifi_connected_generation = 0;
    taskEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(s_wifi_command_mutex);

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret == ESP_OK) {
        ret = esp_wifi_disconnect();
        if (ret == ESP_ERR_WIFI_NOT_CONNECT) {
            taskENTER_CRITICAL(&s_state_lock);
            s_suppress_next_disconnect = false;
            taskEXIT_CRITICAL(&s_state_lock);
            ret = ESP_OK;
        }
    } else {
        taskENTER_CRITICAL(&s_state_lock);
        s_suppress_next_disconnect = false;
        taskEXIT_CRITICAL(&s_state_lock);
    }

    if (ret == ESP_OK) {
        int64_t now_us = wireless_now_us();
        taskENTER_CRITICAL(&s_state_lock);
        s_wifi_connect_requested = true;
        s_status.wifi_retry_count = 0;
        s_status.wifi_connected = false;
        s_status.wifi_state = TEST_WIRELESS_WIFI_CONNECTING;
        s_status.last_error = ESP_OK;
        wireless_copy_string(s_status.current_ssid, sizeof(s_status.current_ssid), ssid);
        wireless_copy_string(s_status.current_ip, sizeof(s_status.current_ip), "--");
        wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), "wifi connect");
        wireless_commit_locked(now_us);
        taskEXIT_CRITICAL(&s_state_lock);
        ret = esp_wifi_connect();
    }

    if (ret != ESP_OK) {
        (void)xSemaphoreTake(s_wifi_command_mutex, portMAX_DELAY);
        wireless_stop_reconnect_timer_locked();
        taskENTER_CRITICAL(&s_state_lock);
        s_wifi_connect_requested = false;
        s_suppress_next_disconnect = false;
        s_wifi_connected_generation = 0;
        s_status.wifi_state = TEST_WIRELESS_WIFI_ERROR;
        taskEXIT_CRITICAL(&s_state_lock);
        xSemaphoreGive(s_wifi_command_mutex);
        wireless_set_error(ret, "wifi connect call fail");
    }
    wireless_wifi_command_release();
    wireless_activity_exit();
    return ret;
}

esp_err_t test_wireless_request_wifi_disconnect(void)
{
    ESP_RETURN_ON_ERROR(wireless_require_initialized(), TAG, "wireless init state invalid");
    ESP_RETURN_ON_FALSE(WIRELESS_WIFI_FEATURE_ENABLED, ESP_ERR_NOT_SUPPORTED, TAG, "WiFi feature disabled");
    ESP_RETURN_ON_FALSE(wireless_activity_enter(true), ESP_ERR_INVALID_STATE, TAG, "wireless is stopping");

    esp_err_t ret = wireless_wifi_command_claim(false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi command busy, disconnect rejected");
        wireless_activity_exit();
        return ret;
    }
    if (!wireless_wifi_action_allowed_locked(false)) {
        wireless_wifi_command_release();
        wireless_activity_exit();
        return ESP_ERR_INVALID_STATE;
    }

    (void)xSemaphoreTake(s_wifi_command_mutex, portMAX_DELAY);
    (void)wireless_next_wifi_request_generation_locked();
    wireless_stop_reconnect_timer_locked();
    taskENTER_CRITICAL(&s_state_lock);
    s_wifi_connect_requested = false;
    s_suppress_next_disconnect = false;
    s_wifi_connected_generation = 0;
    taskEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(s_wifi_command_mutex);

    ret = esp_wifi_disconnect();
    if (ret == ESP_ERR_WIFI_NOT_CONNECT) {
        ret = ESP_OK;
    }
    int64_t now_us = wireless_now_us();
    taskENTER_CRITICAL(&s_state_lock);
    s_status.wifi_connected = false;
    s_status.wifi_state = ret == ESP_OK ? TEST_WIRELESS_WIFI_IDLE : TEST_WIRELESS_WIFI_ERROR;
    s_status.wifi_retry_count = 0;
    s_status.last_error = ret;
    wireless_copy_string(s_status.current_ip, sizeof(s_status.current_ip), "--");
    wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint),
                         ret == ESP_OK ? "wifi disconnect" : "wifi disconnect fail");
    wireless_commit_locked(now_us);
    taskEXIT_CRITICAL(&s_state_lock);
    wireless_wifi_command_release();
    wireless_activity_exit();
    return ret;
}

esp_err_t test_wireless_request_ble_scan(void)
{
    ESP_RETURN_ON_ERROR(wireless_require_initialized(), TAG, "wireless init state invalid");
    ESP_RETURN_ON_FALSE(WIRELESS_BLE_FEATURE_ENABLED, ESP_ERR_NOT_SUPPORTED, TAG, "BLE feature disabled");
    ESP_RETURN_ON_FALSE(wireless_activity_enter(true), ESP_ERR_INVALID_STATE, TAG, "wireless is stopping");
    wireless_set_error(ESP_ERR_NOT_SUPPORTED, "ble scan unsupported");
    wireless_activity_exit();
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t test_wireless_set_ble_advertising(bool enable)
{
    ESP_RETURN_ON_ERROR(wireless_require_initialized(), TAG, "wireless init state invalid");
    ESP_RETURN_ON_FALSE(WIRELESS_BLE_FEATURE_ENABLED, ESP_ERR_NOT_SUPPORTED, TAG, "BLE feature disabled");
    ESP_RETURN_ON_FALSE(wireless_activity_enter(true), ESP_ERR_INVALID_STATE, TAG, "wireless is stopping");
#if WIRELESS_BLE_HOSTED_ENABLED
    esp_err_t ret = enable ? wireless_ble_start_advertising() : wireless_ble_stop_advertising();
#else
    (void)enable;
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    wireless_set_error(ret, "ble cfg disabled");
#endif
    wireless_activity_exit();
    return ret;
}

esp_err_t test_wireless_deinit(void)
{
    int64_t now_us = wireless_now_us();
    taskENTER_CRITICAL(&s_state_lock);
    if (s_status.module.state == TEST_MODULE_STATE_INITIALIZING ||
        s_status.module.state == TEST_MODULE_STATE_STOPPING) {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_actions_enabled = false;
    s_callbacks_enabled = false;
    s_status.module.state = TEST_MODULE_STATE_STOPPING;
    s_status.initialized = false;
    wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint), "wireless stopping");
    wireless_commit_locked(now_us);
    taskEXIT_CRITICAL(&s_state_lock);

    esp_err_t ret = wireless_release_resources();
    now_us = wireless_now_us();
    taskENTER_CRITICAL(&s_state_lock);
    s_wifi_connect_requested = false;
    s_suppress_next_disconnect = false;
    s_wifi_connected_generation = 0;
    s_status.module.state = ret == ESP_OK ? TEST_MODULE_STATE_UNINITIALIZED : TEST_MODULE_STATE_ERROR;
    s_status.last_error = ret;
    s_status.transport_confirmed = false;
    s_status.transport_state = ret == ESP_OK ? TEST_WIRELESS_TRANSPORT_UNINITIALIZED :
                                              TEST_WIRELESS_TRANSPORT_ERROR;
    s_status.wifi_state = ret == ESP_OK ? TEST_WIRELESS_WIFI_UNINITIALIZED : TEST_WIRELESS_WIFI_ERROR;
    s_status.ble_state = ret == ESP_OK ? TEST_WIRELESS_BLE_UNINITIALIZED : TEST_WIRELESS_BLE_ERROR;
    wireless_copy_string(s_status.last_hint, sizeof(s_status.last_hint),
                         ret == ESP_OK ? "wireless stopped" : "wireless stop fail");
    wireless_commit_locked(now_us);
    taskEXIT_CRITICAL(&s_state_lock);
    return ret;
}
