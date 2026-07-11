#include "transport_wifi.h"

#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "frame_pipeline.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "subscreen_display.h"

static const char *TAG = "transport_wifi";
static esp_netif_t *s_sta_netif;
static httpd_handle_t s_server;
static char s_token[33];
static char s_ipv4[16];
static subscreen_network_status_t s_network_status;
static subscreen_display_t *s_display;
static bool s_has_credentials;

static void update_network_status(subscreen_network_status_t status, const char *ip)
{
    s_network_status = status;
    strlcpy(s_ipv4, ip != NULL ? ip : "", sizeof(s_ipv4));
    if (s_display != NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(subscreen_display_set_network_status(s_display, status, s_ipv4));
    }
}

static esp_err_t nvs_get_string(const char *key, char *value, size_t value_size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("companion", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t length = value_size;
    err = nvs_get_str(handle, key, value, &length);
    nvs_close(handle);
    return err;
}

static void nvs_set_string(const char *key, const char *value)
{
    nvs_handle_t handle;
    if (nvs_open("companion", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, key, value);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void ensure_token(void)
{
    if (nvs_get_string("token", s_token, sizeof(s_token)) == ESP_OK && strlen(s_token) == 32) return;
    static const char hex[] = "0123456789abcdef";
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); ++i) { s_token[i * 2] = hex[raw[i] >> 4]; s_token[i * 2 + 1] = hex[raw[i] & 0x0f]; }
    s_token[32] = '\0';
    nvs_set_string("token", s_token);
}

static esp_err_t root_get(httpd_req_t *req)
{
    static const char page[] = "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>Companion Display</title><h1>Companion Display</h1><form method=post action=/api/wifi><label>Wi-Fi SSID <input name=ssid required></label><br><label>Password <input name=password type=password></label><br><button>Connect</button></form>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get(httpd_req_t *req)
{
    char response[256];
    const char *source = frame_pipeline_active_transport() == COMPANION_TRANSPORT_USB ? "usb" :
                         frame_pipeline_active_transport() == COMPANION_TRANSPORT_WIFI ? "wifi" : "none";
    const char *wifi = s_network_status == SUBSCREEN_NETWORK_CONNECTED ? "connected" :
                       s_network_status == SUBSCREEN_NETWORK_CONNECTING ? "connecting" : "setup_required";
    snprintf(response, sizeof(response), "{\"name\":\"companion-display\",\"width\":368,\"height\":448,\"wifi\":\"%s\",\"ipv4\":\"%s\",\"active_transport\":\"%s\",\"token_set\":true}",
             wifi, s_ipv4, source);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= 256) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request");
    char body[256] = {};
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) return ESP_FAIL;
    char ssid[33] = {}, password[65] = {};
    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
    httpd_query_key_value(body, "password", password, sizeof(password));
    nvs_set_string("ssid", ssid);
    nvs_set_string("password", password);
    wifi_config_t config = {};
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &config));
    s_has_credentials = true;
    update_network_status(SUBSCREEN_NETWORK_CONNECTING, NULL);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    return httpd_resp_sendstr(req, "Connecting. You may close this page.");
}

static bool websocket_authorized(httpd_req_t *req)
{
    char query[96] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    char token[40] = {};
    return httpd_query_key_value(query, "token", token, sizeof(token)) == ESP_OK && strcmp(token, s_token) == 0;
}

static esp_err_t stream_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        if (!websocket_authorized(req)) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "pairing token required");
        if (!frame_pipeline_acquire(COMPANION_TRANSPORT_WIFI)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "other transport active");
        ESP_LOGI(TAG, "WebSocket client connected");
        return ESP_OK;
    }
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_BINARY};
    ESP_RETURN_ON_ERROR(httpd_ws_recv_frame(req, &frame, 0), TAG, "frame header");
    if (frame.type == HTTPD_WS_TYPE_CLOSE) { frame_pipeline_release(COMPANION_TRANSPORT_WIFI); return ESP_OK; }
    if (frame.type != HTTPD_WS_TYPE_BINARY || frame.len < sizeof(companion_frame_header_t)) return ESP_ERR_INVALID_SIZE;
    uint8_t *data = malloc(frame.len);
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_NO_MEM, TAG, "websocket buffer");
    frame.payload = data;
    esp_err_t err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err == ESP_OK) {
        const companion_frame_header_t *header = (const companion_frame_header_t *)data;
        if (frame.len != sizeof(*header) + header->payload_size) err = ESP_ERR_INVALID_SIZE;
        else err = frame_pipeline_submit(COMPANION_TRANSPORT_WIFI, header, data + sizeof(*header));
    }
    free(data);
    return err;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_has_credentials) {
            update_network_status(SUBSCREEN_NETWORK_CONNECTING, NULL);
            esp_wifi_connect();
        } else {
            update_network_status(SUBSCREEN_NETWORK_SETUP_REQUIRED, NULL);
        }
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_has_credentials) {
            update_network_status(SUBSCREEN_NETWORK_CONNECTING, NULL);
            esp_wifi_connect();
        }
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        update_network_status(SUBSCREEN_NETWORK_CONNECTED, ip);
        ESP_LOGI(TAG, "Wi-Fi connected: %s", ip);
    }
}

static esp_err_t start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_COMPANION_HTTP_PORT;
    config.max_uri_handlers = 4;
    config.max_open_sockets = 4;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "HTTP server");
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    const httpd_uri_t status = {.uri = "/api/status", .method = HTTP_GET, .handler = status_get};
    const httpd_uri_t wifi = {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post};
    const httpd_uri_t stream = {.uri = "/stream", .method = HTTP_GET, .handler = stream_ws, .is_websocket = true};
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &status);
    httpd_register_uri_handler(s_server, &wifi);
    httpd_register_uri_handler(s_server, &stream);
    return ESP_OK;
}

esp_err_t transport_wifi_init(subscreen_display_t *display)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); err = nvs_flash_init(); }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS");
    ensure_token();
    s_display = display;
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL));
    wifi_config_t ap = {};
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "Companion-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    strlcpy((char *)ap.ap.password, CONFIG_COMPANION_AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK; ap.ap.max_connection = 2; ap.ap.channel = 1;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "AP config");
    char ssid[33] = {}, password[65] = {};
    if (nvs_get_string("ssid", ssid, sizeof(ssid)) == ESP_OK) {
        s_has_credentials = true;
        nvs_get_string("password", password, sizeof(password));
        wifi_config_t sta = {};
        strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, password, sizeof(sta.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &sta);
    } else {
        update_network_status(SUBSCREEN_NETWORK_SETUP_REQUIRED, NULL);
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    ESP_RETURN_ON_ERROR(start_server(), TAG, "HTTP start");
    ESP_LOGI(TAG, "config AP ready; pairing token=%s", s_token);
    return ESP_OK;
}
