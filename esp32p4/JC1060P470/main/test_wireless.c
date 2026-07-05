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
#include "esp_check.h"
#include "esp_event.h"
#include "espressif__esp_hosted/host/esp_hosted.h"
#include "espressif__esp_hosted/host/esp_hosted_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "test_config.h"

static const char *TAG = "test_wireless";

static test_wireless_status_t s_status;
static bool s_event_loop_ready;
static bool s_netif_ready;
static bool s_hosted_ready;
static bool s_hosted_handlers_registered;
static bool s_wifi_driver_ready;
static bool s_wifi_handlers_registered;
static bool s_wifi_started;
static bool s_wifi_connect_requested;
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_hosted_evt_instance;
static esp_event_handler_instance_t s_wifi_evt_instance;
static esp_event_handler_instance_t s_ip_evt_instance;
static test_wireless_ap_record_t s_scan_results[TEST_WIRELESS_SCAN_RESULT_MAX];
static size_t s_scan_result_count;
static EventGroupHandle_t s_hosted_event_group;

#define WIRELESS_HOSTED_EVENT_TRANSPORT_UP_BIT   BIT0
#define WIRELESS_HOSTED_EVENT_CP_INIT_BIT        BIT1
#define WIRELESS_HOSTED_EVENT_FAILURE_BIT        BIT2
#define WIRELESS_HOSTED_EVENT_DOWN_BIT           BIT3
#define WIRELESS_HOSTED_READY_BITS               (WIRELESS_HOSTED_EVENT_TRANSPORT_UP_BIT | WIRELESS_HOSTED_EVENT_CP_INIT_BIT)
#define WIRELESS_HOSTED_WAIT_READY_MS            5000
#define WIRELESS_EXPECTED_CP_TARGET              "esp32c6"
#define WIRELESS_EXPECTED_RESET_GPIO             54

static void wireless_log_expected_config(void)
{
    ESP_LOGI(TAG, "Expected hosted config: target=%s transport=SDIO slot=%d width=%d clk=%d kHz D0=%d D1=%d D2=%d D3=%d CLK=%d CMD=%d RST=%d",
             WIRELESS_EXPECTED_CP_TARGET,
             1,
             TEST_WIRELESS_SDIO_BUS_WIDTH,
             TEST_WIRELESS_SDIO_CLOCK_KHZ,
             TEST_WIRELESS_SDIO_PIN_D0,
             TEST_WIRELESS_SDIO_PIN_D1,
             TEST_WIRELESS_SDIO_PIN_D2,
             TEST_WIRELESS_SDIO_PIN_D3,
             TEST_WIRELESS_SDIO_PIN_CLK,
             TEST_WIRELESS_SDIO_PIN_CMD,
             WIRELESS_EXPECTED_RESET_GPIO);
}

static void wireless_set_hint(const char *hint)
{
    snprintf(s_status.last_hint, sizeof(s_status.last_hint), "%s", hint);
}

static void wireless_set_transport_error(esp_err_t err, const char *hint)
{
    s_status.transport_confirmed = false;
    s_status.transport_state = TEST_WIRELESS_TRANSPORT_ERROR;
    s_status.wifi_state = TEST_WIRELESS_WIFI_ERROR;
    s_status.last_error = err;
    wireless_set_hint(hint);
}

static esp_err_t wireless_validate_hosted_config(void)
{
#if !CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE
    ESP_LOGE(TAG, "Hosted config mismatch: expected SDIO host interface, but current config is not SDIO");
    wireless_log_expected_config();
#if CONFIG_ESP_HOSTED_SPI_HOST_INTERFACE
    ESP_LOGE(TAG, "Current hosted config: target=%s transport=SPI MOSI=%d MISO=%d CLK=%d CS=%d RST=%d",
             CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET,
             CONFIG_ESP_HOSTED_SPI_GPIO_MOSI,
             CONFIG_ESP_HOSTED_SPI_GPIO_MISO,
             CONFIG_ESP_HOSTED_SPI_GPIO_CLK,
             CONFIG_ESP_HOSTED_SPI_GPIO_CS,
             CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE);
#else
    ESP_LOGE(TAG, "Current hosted config: target=%s transport=non-SDIO",
             CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET);
#endif
    wireless_set_transport_error(ESP_ERR_INVALID_STATE, "cfg mismatch: need c6 sdio");
    return ESP_ERR_INVALID_STATE;
#endif

    bool valid = true;

    if (strcmp(CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET, WIRELESS_EXPECTED_CP_TARGET) != 0) {
        ESP_LOGE(TAG, "Hosted config mismatch: target=%s expected=%s",
                 CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET, WIRELESS_EXPECTED_CP_TARGET);
        valid = false;
    }
    if (CONFIG_ESP_HOSTED_SDIO_SLOT != 1) {
        ESP_LOGE(TAG, "Hosted config mismatch: SDIO slot=%d expected=1", CONFIG_ESP_HOSTED_SDIO_SLOT);
        valid = false;
    }
    if (CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH != TEST_WIRELESS_SDIO_BUS_WIDTH) {
        ESP_LOGE(TAG, "Hosted config mismatch: SDIO width=%d expected=%d",
                 CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH, TEST_WIRELESS_SDIO_BUS_WIDTH);
        valid = false;
    }
    if (CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ != TEST_WIRELESS_SDIO_CLOCK_KHZ) {
        ESP_LOGE(TAG, "Hosted config mismatch: SDIO clk=%d expected=%d",
                 CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ, TEST_WIRELESS_SDIO_CLOCK_KHZ);
        valid = false;
    }
    if (CONFIG_ESP_HOSTED_SDIO_PIN_D0 != TEST_WIRELESS_SDIO_PIN_D0 ||
        CONFIG_ESP_HOSTED_SDIO_PIN_D1 != TEST_WIRELESS_SDIO_PIN_D1 ||
        CONFIG_ESP_HOSTED_SDIO_PIN_D2 != TEST_WIRELESS_SDIO_PIN_D2 ||
        CONFIG_ESP_HOSTED_SDIO_PIN_D3 != TEST_WIRELESS_SDIO_PIN_D3 ||
        CONFIG_ESP_HOSTED_SDIO_PIN_CLK != TEST_WIRELESS_SDIO_PIN_CLK ||
        CONFIG_ESP_HOSTED_SDIO_PIN_CMD != TEST_WIRELESS_SDIO_PIN_CMD) {
        ESP_LOGE(TAG, "Hosted config mismatch: SDIO pins current D0=%d D1=%d D2=%d D3=%d CLK=%d CMD=%d",
                 CONFIG_ESP_HOSTED_SDIO_PIN_D0,
                 CONFIG_ESP_HOSTED_SDIO_PIN_D1,
                 CONFIG_ESP_HOSTED_SDIO_PIN_D2,
                 CONFIG_ESP_HOSTED_SDIO_PIN_D3,
                 CONFIG_ESP_HOSTED_SDIO_PIN_CLK,
                 CONFIG_ESP_HOSTED_SDIO_PIN_CMD);
        valid = false;
    }
    if (CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE != WIRELESS_EXPECTED_RESET_GPIO) {
        ESP_LOGE(TAG, "Hosted config mismatch: slave reset gpio=%d expected=%d",
                 CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE, WIRELESS_EXPECTED_RESET_GPIO);
        valid = false;
    }

    if (!valid) {
        wireless_log_expected_config();
        wireless_set_transport_error(ESP_ERR_INVALID_STATE, "cfg mismatch: need c6 sdio");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Hosted transport config: SDIO, target=%s, slot=%d, width=%d, clk=%d kHz",
             CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET,
             CONFIG_ESP_HOSTED_SDIO_SLOT,
             CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH,
             CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ);
    ESP_LOGI(TAG, "Hosted SDIO pins: D0=%d D1=%d D2=%d D3=%d CLK=%d CMD=%d RST=%d",
             CONFIG_ESP_HOSTED_SDIO_PIN_D0,
             CONFIG_ESP_HOSTED_SDIO_PIN_D1,
             CONFIG_ESP_HOSTED_SDIO_PIN_D2,
             CONFIG_ESP_HOSTED_SDIO_PIN_D3,
             CONFIG_ESP_HOSTED_SDIO_PIN_CLK,
             CONFIG_ESP_HOSTED_SDIO_PIN_CMD,
             CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE);
    return ESP_OK;
}

static esp_err_t wireless_require_initialized(void)
{
    ESP_RETURN_ON_FALSE(s_status.initialized, ESP_ERR_INVALID_STATE, TAG,
                        "wireless not initialized, call test_wireless_init first");
    return ESP_OK;
}

static esp_err_t wireless_require_transport_confirmed(void)
{
    ESP_RETURN_ON_FALSE(s_status.transport_confirmed, ESP_ERR_INVALID_STATE, TAG,
                        "hosted transport not confirmed");
    return ESP_OK;
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

static void wireless_set_wifi_credentials(const char *ssid, const char *password)
{
    snprintf(s_status.current_ssid, sizeof(s_status.current_ssid), "%s", ssid != NULL ? ssid : "");
    (void)password;
}

static void wireless_update_ip_string(const esp_ip4_addr_t *ip)
{
    if (ip == NULL) {
        snprintf(s_status.current_ip, sizeof(s_status.current_ip), "--");
        return;
    }

    snprintf(s_status.current_ip, sizeof(s_status.current_ip), IPSTR, IP2STR(ip));
}

static bool wireless_reason_is_no_ap_found(uint8_t reason)
{
    return reason == WIFI_REASON_NO_AP_FOUND ||
           reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

static void wireless_refresh_rssi(void)
{
    wifi_ap_record_t ap_info = {0};
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (ret == ESP_OK) {
        s_status.wifi_rssi = ap_info.rssi;
    }
}

static bool wireless_auth_mode_is_secured(wifi_auth_mode_t authmode)
{
    return authmode != WIFI_AUTH_OPEN;
}

static const char *wireless_hosted_event_to_hint(int32_t event_id)
{
    switch (event_id) {
    case ESP_HOSTED_EVENT_CP_INIT:
        return "cp init";
    case ESP_HOSTED_EVENT_CP_HEARTBEAT:
        return "cp heartbeat";
    case ESP_HOSTED_EVENT_TRANSPORT_FAILURE:
        return "transport fail";
    case ESP_HOSTED_EVENT_TRANSPORT_UP:
        return "transport up";
    case ESP_HOSTED_EVENT_TRANSPORT_DOWN:
        return "transport down";
    case ESP_HOSTED_EVENT_MEM_MONITOR:
        return "cp mem";
    default:
        return "hosted event";
    }
}

static void wireless_record_cp_info(void)
{
    uint32_t cp_chip_id = 0;
    char cp_target_name[TEST_WIRELESS_CP_TARGET_MAX_LEN] = {0};
    esp_hosted_coprocessor_fwver_t fwver = {0};

    esp_err_t ret = esp_hosted_get_cp_info(&cp_chip_id, cp_target_name, sizeof(cp_target_name));
    if (ret == ESP_OK) {
        s_status.cp_chip_id = cp_chip_id;
        snprintf(s_status.cp_target_name, sizeof(s_status.cp_target_name), "%s", cp_target_name);
        s_status.hosted_cp_info_error = ESP_OK;
    } else {
        s_status.cp_chip_id = 0;
        snprintf(s_status.cp_target_name, sizeof(s_status.cp_target_name), "%s", "unknown");
        s_status.hosted_cp_info_error = ret;
        ESP_LOGW(TAG, "esp_hosted_get_cp_info failed: %s", esp_err_to_name(ret));
    }

    ret = esp_hosted_get_coprocessor_fwversion(&fwver);
    if (ret == ESP_OK) {
        snprintf(s_status.cp_fw_version, sizeof(s_status.cp_fw_version), "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 fwver.major1, fwver.minor1, fwver.patch1);
        s_status.hosted_fw_version_error = ESP_OK;
        s_status.rpc_metadata_ready = true;
        if (s_status.transport_confirmed) {
            wireless_set_hint("cp verified");
        }
    } else {
        snprintf(s_status.cp_fw_version, sizeof(s_status.cp_fw_version), "%s", "unread");
        s_status.hosted_fw_version_error = ret;
        s_status.rpc_metadata_ready = false;
        if (s_status.transport_confirmed) {
            wireless_set_hint("cp meta fail");
        }
        ESP_LOGW(TAG, "esp_hosted_get_coprocessor_fwversion failed: %s", esp_err_to_name(ret));
    }
}

static esp_err_t wireless_wait_hosted_ready(void)
{
    ESP_RETURN_ON_FALSE(s_hosted_event_group != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "hosted event group not ready");

    EventBits_t bits = xEventGroupWaitBits(s_hosted_event_group,
                                           WIRELESS_HOSTED_EVENT_TRANSPORT_UP_BIT |
                                           WIRELESS_HOSTED_EVENT_FAILURE_BIT |
                                           WIRELESS_HOSTED_EVENT_DOWN_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WIRELESS_HOSTED_WAIT_READY_MS));

    if ((bits & WIRELESS_HOSTED_EVENT_FAILURE_BIT) != 0U) {
        wireless_set_transport_error(ESP_FAIL, "transport fail evt");
        return ESP_FAIL;
    }

    if ((bits & WIRELESS_HOSTED_EVENT_DOWN_BIT) != 0U) {
        wireless_set_transport_error(ESP_FAIL, "transport down evt");
        return ESP_FAIL;
    }

    if ((bits & WIRELESS_HOSTED_EVENT_TRANSPORT_UP_BIT) == 0U) {
        wireless_set_transport_error(ESP_ERR_TIMEOUT, "transport wait timeout");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void wireless_clear_scan_results(void)
{
    memset(s_scan_results, 0, sizeof(s_scan_results));
    s_scan_result_count = 0;
    s_status.wifi_ap_count = 0;
    s_status.wifi_scan_results_valid = false;
}

static void wireless_update_scan_results(void)
{
    uint16_t ap_count = 0;
    uint16_t fetch_count = TEST_WIRELESS_SCAN_RESULT_MAX;
    wifi_ap_record_t ap_info[TEST_WIRELESS_SCAN_RESULT_MAX] = {0};

    esp_err_t ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        wireless_clear_scan_results();
        s_status.wifi_state = TEST_WIRELESS_WIFI_ERROR;
        s_status.last_error = ret;
        wireless_set_hint("scan get num fail");
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(ret));
        return;
    }

    if (fetch_count > ap_count) {
        fetch_count = ap_count;
    }

    if (fetch_count > 0) {
        ret = esp_wifi_scan_get_ap_records(&fetch_count, ap_info);
        if (ret != ESP_OK) {
            wireless_clear_scan_results();
            s_status.wifi_state = TEST_WIRELESS_WIFI_ERROR;
            s_status.last_error = ret;
            wireless_set_hint("scan get rec fail");
            ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    memset(s_scan_results, 0, sizeof(s_scan_results));
    s_scan_result_count = fetch_count;
    s_status.wifi_ap_count = ap_count;
    s_status.wifi_scan_results_valid = true;
    s_status.wifi_scan_generation++;
    s_status.last_error = ESP_OK;

    for (size_t i = 0; i < fetch_count; i++) {
        snprintf(s_scan_results[i].ssid, sizeof(s_scan_results[i].ssid), "%.*s",
                 (int)sizeof(ap_info[i].ssid), ap_info[i].ssid);
        s_scan_results[i].rssi = ap_info[i].rssi;
        s_scan_results[i].secured = wireless_auth_mode_is_secured(ap_info[i].authmode);
    }

    if (ap_count == 0) {
        wireless_set_hint("scan done 0 ap");
    } else {
        wireless_set_hint("wifi scan done");
    }
}

static void wireless_hosted_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base != ESP_HOSTED_EVENT) {
        return;
    }

    s_status.hosted_last_event = event_id;
    ESP_LOGI(TAG, "hosted event: %" PRIi32, event_id);

    if (event_id == ESP_HOSTED_EVENT_TRANSPORT_UP) {
        if (s_hosted_event_group != NULL) {
            xEventGroupSetBits(s_hosted_event_group, WIRELESS_HOSTED_EVENT_TRANSPORT_UP_BIT);
        }
        s_status.transport_up_received = true;
        s_status.transport_confirmed = true;
        s_status.transport_state = TEST_WIRELESS_TRANSPORT_READY;
        s_status.last_error = ESP_OK;
        wireless_set_hint("transport up");
    } else if (event_id == ESP_HOSTED_EVENT_TRANSPORT_DOWN) {
        if (s_hosted_event_group != NULL) {
            xEventGroupSetBits(s_hosted_event_group, WIRELESS_HOSTED_EVENT_DOWN_BIT);
            xEventGroupClearBits(s_hosted_event_group, WIRELESS_HOSTED_READY_BITS);
        }
        wireless_set_transport_error(ESP_FAIL, "transport down");
    } else if (event_id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE) {
        if (s_hosted_event_group != NULL) {
            xEventGroupSetBits(s_hosted_event_group, WIRELESS_HOSTED_EVENT_FAILURE_BIT);
            xEventGroupClearBits(s_hosted_event_group, WIRELESS_HOSTED_READY_BITS);
        }
        wireless_set_transport_error(ESP_FAIL, "transport fail");
    } else if (event_id == ESP_HOSTED_EVENT_CP_INIT) {
        if (s_hosted_event_group != NULL) {
            xEventGroupSetBits(s_hosted_event_group, WIRELESS_HOSTED_EVENT_CP_INIT_BIT);
        }
        s_status.cp_init_received = true;
        const esp_hosted_event_init_t *init_event = (const esp_hosted_event_init_t *)event_data;
        if (init_event != NULL) {
            ESP_LOGI(TAG, "co-processor reset reason: %d", init_event->reason);
        }
        wireless_set_hint("cp init");
    } else {
        wireless_set_hint(wireless_hosted_event_to_hint(event_id));
    }
}

static void wireless_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_status.wifi_state = TEST_WIRELESS_WIFI_IDLE;
        s_status.last_error = ESP_OK;
        wireless_set_hint("wifi sta start");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        const wifi_event_sta_scan_done_t *scan_done = (const wifi_event_sta_scan_done_t *)event_data;
        if (scan_done != NULL) {
            ESP_LOGI(TAG, "scan done status=%" PRIu32 " number=%" PRIu8 " scan_id=%" PRIu8,
                     scan_done->status, scan_done->number, scan_done->scan_id);
            if (scan_done->status != 0U) {
                wireless_clear_scan_results();
                s_status.wifi_state = TEST_WIRELESS_WIFI_ERROR;
                s_status.last_error = ESP_FAIL;
                wireless_set_hint("scan done fail");
                return;
            }
        }
        wireless_update_scan_results();
        if (s_status.last_error == ESP_OK) {
            s_status.wifi_state = TEST_WIRELESS_WIFI_IDLE;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *connected = (wifi_event_sta_connected_t *)event_data;
        s_status.wifi_connected = true;
        s_status.wifi_retry_count = 0;
        s_status.wifi_rssi = 0;
        snprintf(s_status.current_ssid, sizeof(s_status.current_ssid), "%.*s",
                 connected != NULL ? (int)connected->ssid_len : 0,
                 connected != NULL ? connected->ssid : (uint8_t *)"");
        wireless_update_ip_string(NULL);
        s_status.wifi_state = TEST_WIRELESS_WIFI_CONNECTED;
        s_status.last_error = ESP_OK;
        wireless_set_hint("wifi connected");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconnected = (wifi_event_sta_disconnected_t *)event_data;
        s_status.wifi_connected = false;
        s_status.wifi_rssi = disconnected != NULL ? disconnected->rssi : 0;
        wireless_update_ip_string(NULL);

        if (!s_wifi_connect_requested) {
            s_status.wifi_state = TEST_WIRELESS_WIFI_IDLE;
            s_status.last_error = ESP_OK;
            wireless_set_hint("wifi idle");
            return;
        }

        if (s_status.wifi_retry_count < CONFIG_EXAMPLE_WIRELESS_MAXIMUM_RETRY) {
            esp_err_t ret = esp_wifi_connect();
            if (ret == ESP_OK) {
                s_status.wifi_retry_count++;
                s_status.wifi_state = TEST_WIRELESS_WIFI_CONNECTING;
                s_status.last_error = ESP_OK;
                wireless_set_hint("wifi reconnect");
                return;
            }
            s_status.last_error = ret;
        } else {
            s_status.last_error = ESP_FAIL;
        }

        s_status.wifi_state = TEST_WIRELESS_WIFI_ERROR;
        wireless_set_hint(disconnected != NULL && wireless_reason_is_no_ap_found(disconnected->reason) ?
                          "no ap found" : "wifi disconnect");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got_ip = (ip_event_got_ip_t *)event_data;
        s_status.wifi_connected = true;
        s_status.wifi_state = TEST_WIRELESS_WIFI_CONNECTED;
        s_status.last_error = ESP_OK;
        if (got_ip != NULL) {
            wireless_update_ip_string(&got_ip->ip_info.ip);
        } else {
            wireless_update_ip_string(NULL);
        }
        wireless_refresh_rssi();
        wireless_set_hint("ip got");
    }
}

static esp_err_t wireless_prepare_stack(void)
{
    if (!s_netif_ready) {
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

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_ERR_NO_MEM, TAG, "create wifi sta netif failed");
    }

    return ESP_OK;
}

static esp_err_t wireless_prepare_hosted(void)
{
    if (s_hosted_event_group == NULL) {
        s_hosted_event_group = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_hosted_event_group != NULL, ESP_ERR_NO_MEM, TAG, "create hosted event group failed");
    }

    if (!s_hosted_handlers_registered) {
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(ESP_HOSTED_EVENT,
                                                                ESP_EVENT_ANY_ID,
                                                                &wireless_hosted_event_handler,
                                                                NULL,
                                                                &s_hosted_evt_instance),
                            TAG, "register hosted event failed");
        s_hosted_handlers_registered = true;
    }

    if (!s_hosted_ready) {
        /* esp_hosted is auto-initialized by the component constructor before app_main(). */
        ESP_LOGI(TAG, "Use pre-initialized esp_hosted instance from constructor");
        s_hosted_ready = true;
    }

    s_status.transport_state = TEST_WIRELESS_TRANSPORT_CONNECTING;
    s_status.transport_confirmed = false;
    s_status.rpc_metadata_ready = false;
    s_status.transport_up_received = false;
    s_status.cp_init_received = false;
    s_status.hosted_cp_info_error = ESP_OK;
    s_status.hosted_fw_version_error = ESP_OK;
    s_status.last_error = ESP_OK;
    wireless_set_hint("hosted connect");
    xEventGroupClearBits(s_hosted_event_group,
                         WIRELESS_HOSTED_READY_BITS |
                         WIRELESS_HOSTED_EVENT_FAILURE_BIT |
                         WIRELESS_HOSTED_EVENT_DOWN_BIT);

    ESP_RETURN_ON_ERROR(esp_hosted_connect_to_slave(), TAG, "esp_hosted_connect_to_slave failed");

    esp_err_t wait_ret = wireless_wait_hosted_ready();
    if (wait_ret != ESP_OK) {
        ESP_RETURN_ON_ERROR(wait_ret, TAG, "hosted transport wait failed");
    }

    ESP_LOGI(TAG, "Hosted transport ready, continue WiFi path");
    wireless_record_cp_info();
    if (!s_status.rpc_metadata_ready) {
        ESP_LOGW(TAG, "Hosted CP metadata unavailable, continue with WiFi path");
    }
    return ESP_OK;
}

static esp_err_t wireless_prepare_wifi_driver(void)
{
    if (!s_wifi_driver_ready) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t ret = esp_wifi_init(&cfg);
        if (ret == ESP_ERR_WIFI_INIT_STATE) {
            s_wifi_driver_ready = true;
        } else {
            ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_init failed");
            s_wifi_driver_ready = true;
        }
    }

    if (!s_wifi_handlers_registered) {
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT,
                                                                ESP_EVENT_ANY_ID,
                                                                &wireless_event_handler,
                                                                NULL,
                                                                &s_wifi_evt_instance),
                            TAG, "register wifi event failed");
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
                                                                IP_EVENT_STA_GOT_IP,
                                                                &wireless_event_handler,
                                                                NULL,
                                                                &s_ip_evt_instance),
                            TAG, "register ip event failed");
        s_wifi_handlers_registered = true;
    }

    return ESP_OK;
}

static esp_err_t wireless_start_wifi(void)
{
    if (s_wifi_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
    s_wifi_started = true;
    return ESP_OK;
}

esp_err_t test_wireless_init(void)
{
    if (s_status.initialized) {
        return ESP_OK;
    }

    s_status = (test_wireless_status_t) {
        .init_attempted = true,
        .hosted_stack_available = true,
        .wifi_feature_enabled = CONFIG_EXAMPLE_WIRELESS_ENABLE_WIFI,
        .ble_feature_enabled = CONFIG_EXAMPLE_WIRELESS_ENABLE_BLE,
        .sdio_board_link_defined = true,
        .ble_advertising_enabled = false,
        .wifi_connected = false,
        .transport_confirmed = false,
        .rpc_metadata_ready = false,
        .cp_init_received = false,
        .transport_up_received = false,
        .transport_state = TEST_WIRELESS_TRANSPORT_IDLE,
        .wifi_state = CONFIG_EXAMPLE_WIRELESS_ENABLE_WIFI ? TEST_WIRELESS_WIFI_IDLE :
                                                           TEST_WIRELESS_WIFI_NOT_AVAILABLE,
        .ble_state = CONFIG_EXAMPLE_WIRELESS_ENABLE_BLE ? TEST_WIRELESS_BLE_NOT_AVAILABLE :
                                                          TEST_WIRELESS_BLE_UNINITIALIZED,
        .hosted_last_event = -1,
        .cp_chip_id = 0,
        .wifi_ap_count = 0,
        .ble_device_count = 0,
        .wifi_scan_generation = 0,
        .wifi_rssi = 0,
        .wifi_retry_count = 0,
        .wifi_scan_results_valid = false,
        .hosted_cp_info_error = ESP_OK,
        .hosted_fw_version_error = ESP_OK,
        .last_error = ESP_OK,
    };
    snprintf(s_status.current_ip, sizeof(s_status.current_ip), "--");
    snprintf(s_status.cp_target_name, sizeof(s_status.cp_target_name), "%s", "--");
    snprintf(s_status.cp_fw_version, sizeof(s_status.cp_fw_version), "%s", "--");
    wireless_clear_scan_results();
    wireless_set_hint("wifi hosted init");
    s_hosted_ready = true;

    esp_err_t ret = wireless_validate_hosted_config();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Hosted config validation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = wireless_prepare_stack();
    if (ret != ESP_OK) {
        wireless_set_transport_error(ret, "stack fail");
        return ret;
    }

    ret = wireless_prepare_hosted();
    if (ret != ESP_OK) {
        wireless_set_transport_error(ret, s_status.last_hint[0] != '\0' ? s_status.last_hint : "hosted connect fail");
        return ret;
    }

    ret = wireless_prepare_wifi_driver();
    if (ret != ESP_OK) {
        wireless_set_transport_error(ret, "wifi init fail");
        return ret;
    }

    ret = wireless_start_wifi();
    if (ret != ESP_OK) {
        wireless_set_transport_error(ret, "wifi start fail");
        return ret;
    }

    s_status.initialized = true;
    s_status.transport_state = TEST_WIRELESS_TRANSPORT_READY;
    s_status.transport_confirmed = true;
    if (s_status.ble_feature_enabled) {
        wireless_set_hint("wifi ok ble na");
    } else {
        wireless_set_hint("wifi ready");
    }

    ESP_LOGI(TAG, "WiFi hosted path verified on P4<->C6 SDIO");
    ESP_LOGI(TAG, "P4<->C6 SDIO pins: D0=%d D1=%d D2=%d D3=%d CLK=%d CMD=%d",
             TEST_WIRELESS_SDIO_PIN_D0,
             TEST_WIRELESS_SDIO_PIN_D1,
             TEST_WIRELESS_SDIO_PIN_D2,
             TEST_WIRELESS_SDIO_PIN_D3,
             TEST_WIRELESS_SDIO_PIN_CLK,
             TEST_WIRELESS_SDIO_PIN_CMD);
    ESP_LOGI(TAG, "co-processor target=%s chip=0x%" PRIx32 " fw=%s rpc_meta=%d",
             s_status.cp_target_name,
             s_status.cp_chip_id,
             s_status.cp_fw_version,
             s_status.rpc_metadata_ready);

    if (s_status.ble_feature_enabled) {
        ESP_LOGW(TAG, "BLE hosted path is not integrated yet; WiFi is available, BLE stays disabled");
    }
    return ESP_OK;
}

esp_err_t test_wireless_run(void)
{
    return wireless_require_initialized();
}

esp_err_t test_wireless_get_status(test_wireless_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != NULL, ESP_ERR_INVALID_ARG, TAG, "out_status is NULL");
    *out_status = s_status;
    return ESP_OK;
}

esp_err_t test_wireless_get_scan_results(test_wireless_ap_record_t *records, size_t max_records, size_t *out_count)
{
    ESP_RETURN_ON_ERROR(wireless_require_initialized(), TAG, "wireless init state invalid");
    ESP_RETURN_ON_FALSE(out_count != NULL, ESP_ERR_INVALID_ARG, TAG, "out_count is NULL");

    size_t copy_count = s_scan_result_count;
    if (copy_count > max_records) {
        copy_count = max_records;
    }

    if (copy_count > 0) {
        ESP_RETURN_ON_FALSE(records != NULL, ESP_ERR_INVALID_ARG, TAG, "records is NULL");
        memcpy(records, s_scan_results, copy_count * sizeof(test_wireless_ap_record_t));
    }

    *out_count = copy_count;
    return ESP_OK;
}

esp_err_t test_wireless_request_wifi_scan(void)
{
    ESP_RETURN_ON_ERROR(wireless_require_initialized(), TAG, "wireless init state invalid");
    ESP_RETURN_ON_FALSE(s_status.wifi_feature_enabled, ESP_ERR_NOT_SUPPORTED, TAG, "WiFi feature disabled");
    ESP_RETURN_ON_ERROR(wireless_require_transport_confirmed(), TAG, "transport not confirmed");

    s_status.wifi_state = TEST_WIRELESS_WIFI_SCANNING;
    wireless_clear_scan_results();
    wireless_set_hint("wifi scan start");
    esp_err_t ret = esp_wifi_scan_start(NULL, false);
    if (ret != ESP_OK) {
        s_status.wifi_state = TEST_WIRELESS_WIFI_ERROR;
        s_status.last_error = ret;
        wireless_set_hint("scan start fail");
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_status.last_error = ESP_OK;
    return ESP_OK;
}

esp_err_t test_wireless_request_wifi_connect(const char *ssid, const char *password)
{
    ESP_RETURN_ON_ERROR(wireless_require_initialized(), TAG, "wireless init state invalid");
    ESP_RETURN_ON_FALSE(s_status.wifi_feature_enabled, ESP_ERR_NOT_SUPPORTED, TAG, "WiFi feature disabled");
    ESP_RETURN_ON_ERROR(wireless_require_transport_confirmed(), TAG, "transport not confirmed");
    ESP_RETURN_ON_FALSE(ssid != NULL, ESP_ERR_INVALID_ARG, TAG, "ssid is NULL");
    ESP_RETURN_ON_FALSE(ssid[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "ssid is empty");

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = wireless_auth_mode_threshold(),
        },
    };

    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", ssid);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", password != NULL ? password : "");

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "esp_wifi_set_config failed");
    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_disconnect before connect failed");
    }

    s_wifi_connect_requested = true;
    s_status.wifi_retry_count = 0;
    s_status.wifi_connected = false;
    s_status.wifi_state = TEST_WIRELESS_WIFI_CONNECTING;
    s_status.last_error = ESP_OK;
    wireless_set_wifi_credentials(ssid, password);
    wireless_update_ip_string(NULL);
    wireless_set_hint("wifi connect");

    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "esp_wifi_connect failed");
    return ESP_OK;
}

esp_err_t test_wireless_request_wifi_disconnect(void)
{
    ESP_RETURN_ON_ERROR(wireless_require_initialized(), TAG, "wireless init state invalid");
    ESP_RETURN_ON_FALSE(s_status.wifi_feature_enabled, ESP_ERR_NOT_SUPPORTED, TAG, "WiFi feature disabled");

    s_wifi_connect_requested = false;
    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_disconnect failed");
    }
    s_status.wifi_connected = false;
    s_status.wifi_state = TEST_WIRELESS_WIFI_IDLE;
    s_status.wifi_retry_count = 0;
    s_status.last_error = ESP_OK;
    wireless_update_ip_string(NULL);
    wireless_set_hint("wifi disconnect");
    return ESP_OK;
}

esp_err_t test_wireless_request_ble_scan(void)
{
    ESP_RETURN_ON_ERROR(wireless_require_initialized(), TAG, "wireless init state invalid");
    ESP_RETURN_ON_FALSE(s_status.ble_feature_enabled, ESP_ERR_NOT_SUPPORTED, TAG, "BLE feature disabled");
    s_status.ble_state = TEST_WIRELESS_BLE_NOT_AVAILABLE;
    s_status.last_error = ESP_ERR_NOT_SUPPORTED;
    wireless_set_hint("ble na");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t test_wireless_set_ble_advertising(bool enable)
{
    ESP_RETURN_ON_ERROR(wireless_require_initialized(), TAG, "wireless init state invalid");
    ESP_RETURN_ON_FALSE(s_status.ble_feature_enabled, ESP_ERR_NOT_SUPPORTED, TAG, "BLE feature disabled");
    s_status.ble_advertising_enabled = enable;
    s_status.ble_state = TEST_WIRELESS_BLE_NOT_AVAILABLE;
    s_status.last_error = ESP_ERR_NOT_SUPPORTED;
    wireless_set_hint("ble na");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t test_wireless_deinit(void)
{
    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }

    if (s_wifi_handlers_registered) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_evt_instance);
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_evt_instance);
        s_wifi_handlers_registered = false;
    }

    if (s_wifi_driver_ready) {
        esp_wifi_deinit();
        s_wifi_driver_ready = false;
    }

    if (s_hosted_handlers_registered) {
        esp_event_handler_instance_unregister(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, s_hosted_evt_instance);
        s_hosted_handlers_registered = false;
    }

    /* esp_hosted lifetime is owned by the component constructor/destructor. */
    s_hosted_ready = false;

    if (s_hosted_event_group != NULL) {
        vEventGroupDelete(s_hosted_event_group);
        s_hosted_event_group = NULL;
    }

    if (s_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    wireless_clear_scan_results();
    s_wifi_connect_requested = false;
    s_status = (test_wireless_status_t) {0};
    return ESP_OK;
}
