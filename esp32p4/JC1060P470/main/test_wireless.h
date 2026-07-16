/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "test_common.h"

#define TEST_WIRELESS_SSID_MAX_LEN              32
#define TEST_WIRELESS_PASSWORD_MAX_LEN          63
#define TEST_WIRELESS_IPV4_STR_LEN              16
#define TEST_WIRELESS_HINT_MAX_LEN              48
#define TEST_WIRELESS_SCAN_RESULT_MAX           12
#define TEST_WIRELESS_CP_TARGET_MAX_LEN         24
#define TEST_WIRELESS_FW_VERSION_MAX_LEN        24
#define TEST_WIRELESS_BLE_NAME_MAX_LEN          32

typedef struct {
    char ssid[TEST_WIRELESS_SSID_MAX_LEN + 1];
    int8_t rssi;
    bool secured;
} test_wireless_ap_record_t;

typedef enum {
    TEST_WIRELESS_TRANSPORT_UNINITIALIZED = 0,
    TEST_WIRELESS_TRANSPORT_NOT_AVAILABLE,
    TEST_WIRELESS_TRANSPORT_IDLE,
    TEST_WIRELESS_TRANSPORT_CONNECTING,
    TEST_WIRELESS_TRANSPORT_READY,
    TEST_WIRELESS_TRANSPORT_ERROR,
} test_wireless_transport_state_t;

typedef enum {
    TEST_WIRELESS_WIFI_UNINITIALIZED = 0,
    TEST_WIRELESS_WIFI_NOT_AVAILABLE,
    TEST_WIRELESS_WIFI_IDLE,
    TEST_WIRELESS_WIFI_SCANNING,
    TEST_WIRELESS_WIFI_CONNECTING,
    TEST_WIRELESS_WIFI_CONNECTED,
    TEST_WIRELESS_WIFI_ERROR,
} test_wireless_wifi_state_t;

typedef enum {
    TEST_WIRELESS_BLE_UNINITIALIZED = 0,
    TEST_WIRELESS_BLE_NOT_AVAILABLE,
    TEST_WIRELESS_BLE_IDLE,
    TEST_WIRELESS_BLE_SCANNING,
    TEST_WIRELESS_BLE_ADVERTISING,
    TEST_WIRELESS_BLE_ERROR,
} test_wireless_ble_state_t;

typedef struct {
    test_module_meta_t module;
    bool initialized;
    bool init_attempted;
    bool hosted_stack_available;
    bool wifi_feature_enabled;
    bool ble_feature_enabled;
    bool sdio_board_link_defined;
    bool ble_advertising_enabled;
    bool ble_connected;
    bool ble_host_synced;
    bool wifi_connected;
    bool transport_confirmed;
    bool rpc_metadata_ready;
    bool cp_init_received;
    bool transport_up_received;
    test_wireless_transport_state_t transport_state;
    test_wireless_wifi_state_t wifi_state;
    test_wireless_ble_state_t ble_state;
    int32_t hosted_last_event;
    uint32_t cp_chip_id;
    uint32_t wifi_ap_count;
    uint32_t ble_device_count;
    uint32_t wifi_scan_generation;
    int64_t wifi_scan_updated_at_us;
    uint8_t ble_test_value;
    int wifi_rssi;
    uint32_t wifi_retry_count;
    bool wifi_scan_results_valid;
    esp_err_t hosted_cp_info_error;
    esp_err_t hosted_fw_version_error;
    esp_err_t last_error;
    char current_ssid[TEST_WIRELESS_SSID_MAX_LEN + 1];
    char current_ip[TEST_WIRELESS_IPV4_STR_LEN];
    char cp_target_name[TEST_WIRELESS_CP_TARGET_MAX_LEN];
    char cp_fw_version[TEST_WIRELESS_FW_VERSION_MAX_LEN];
    char ble_device_name[TEST_WIRELESS_BLE_NAME_MAX_LEN];
    char last_hint[TEST_WIRELESS_HINT_MAX_LEN];
} test_wireless_status_t;

typedef struct {
    uint32_t generation;
    int64_t updated_at_us;
    uint32_t total_count;
    size_t record_count;
    bool valid;
    test_wireless_ap_record_t records[TEST_WIRELESS_SCAN_RESULT_MAX];
} test_wireless_scan_snapshot_t;

esp_err_t test_wireless_init(void);
/* Records a prerequisite error without creating or releasing wireless resources. */
esp_err_t test_wireless_report_dependency_failure(esp_err_t error);
esp_err_t test_wireless_run(void);
esp_err_t test_wireless_get_status(test_wireless_status_t *out_status);
esp_err_t test_wireless_get_scan_snapshot(test_wireless_scan_snapshot_t *out_snapshot);
esp_err_t test_wireless_get_scan_results(test_wireless_ap_record_t *records, size_t max_records, size_t *out_count);
esp_err_t test_wireless_request_wifi_scan(void);
esp_err_t test_wireless_request_wifi_connect(const char *ssid, const char *password);
esp_err_t test_wireless_request_wifi_disconnect(void);
esp_err_t test_wireless_request_ble_scan(void);
esp_err_t test_wireless_set_ble_advertising(bool enable);
esp_err_t test_wireless_deinit(void);
