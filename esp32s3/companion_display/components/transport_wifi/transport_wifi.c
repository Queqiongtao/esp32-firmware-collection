#include "transport_wifi.h"

#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "frame_pipeline.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "subscreen_display.h"

#define CONTROL_PROTOCOL_VERSION 1
#define UDP_MAGIC 0x31554443UL /* CDU1, little-endian */
#define UDP_STREAM_VIDEO 1
#define UDP_MAX_PAYLOAD 1200U
#define UDP_MAX_FRAGMENTS 128U
#define UDP_REASSEMBLY_TIMEOUT_US (250 * 1000)
#define UDP_SESSION_TIMEOUT_US (5 * 1000 * 1000)
#define FRAME_LIMIT ((size_t)CONFIG_COMPANION_FRAME_LIMIT_KB * 1024U)

typedef enum { VIDEO_TRANSPORT_NONE, VIDEO_TRANSPORT_TCP, VIDEO_TRANSPORT_UDP } video_transport_t;
typedef enum {
    UDP_REJECT_NONE,
    UDP_REJECT_INACTIVE,
    UDP_REJECT_SOURCE,
    UDP_REJECT_HEADER,
    UDP_REJECT_SESSION,
    UDP_REJECT_FRAGMENT,
    UDP_REJECT_LENGTH,
} udp_reject_reason_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t stream_type;
    uint16_t header_size;
    uint32_t session_id;
    uint32_t session_token;
    uint32_t frame_sequence;
    uint32_t frame_length;
    uint16_t fragment_index;
    uint16_t fragment_count;
    uint32_t fragment_offset;
    uint16_t payload_length;
    uint16_t reserved;
} companion_udp_header_t;

typedef struct {
    bool active;
    bool negotiated;
    bool started;
    int ws_fd;
    video_transport_t video_transport;
    uint32_t session_id;
    uint32_t session_token;
    struct in_addr peer_addr;
    bool peer_addr_bound;
    int64_t last_control_us;
    int64_t last_udp_us;
    uint32_t dropped_frames;
    uint32_t udp_rx_packets;
    uint32_t udp_accepted_fragments;
    uint32_t udp_rejected_packets;
    uint32_t udp_completed_frames;
    uint32_t udp_submitted_frames;
    uint32_t udp_submit_failures;
    uint32_t udp_reassembly_timeouts;
    udp_reject_reason_t udp_last_reject;
    uint32_t reassembly_sequence;
    uint32_t reassembly_length;
    uint16_t reassembly_fragments;
    uint64_t received_fragments[2];
    int64_t reassembly_started_us;
} stream_session_t;

static const char *TAG = "transport_wifi";
static esp_netif_t *s_sta_netif;
static httpd_handle_t s_server;
static int s_udp_socket = -1;
static char s_token[7];
static char s_ipv4[16];
static subscreen_network_status_t s_network_status;
static subscreen_display_t *s_display;
static bool s_has_credentials;
static SemaphoreHandle_t s_session_lock;
static stream_session_t s_session;
static uint8_t *s_udp_reassembly;
static int64_t s_last_udp_summary_us;
static int64_t s_last_udp_reject_log_us;
static udp_reject_reason_t s_last_logged_reject;

static void update_network_status(subscreen_network_status_t status, const char *ip)
{
    s_network_status = status;
    strlcpy(s_ipv4, ip != NULL ? ip : "", sizeof(s_ipv4));
    if (s_display != NULL) ESP_ERROR_CHECK_WITHOUT_ABORT(subscreen_display_set_network_status(s_display, status, s_ipv4, s_token));
}

static void update_transport_status(subscreen_transport_status_t status)
{
    if (s_display != NULL) ESP_ERROR_CHECK_WITHOUT_ABORT(subscreen_display_set_transport_status(s_display, status));
}

static companion_transport_t pipeline_transport(video_transport_t transport)
{
    return transport == VIDEO_TRANSPORT_UDP ? COMPANION_TRANSPORT_UDP_VIDEO : COMPANION_TRANSPORT_TCP_VIDEO;
}

static const char *video_transport_name(video_transport_t transport)
{
    return transport == VIDEO_TRANSPORT_UDP ? "udp" : transport == VIDEO_TRANSPORT_TCP ? "tcp" : "none";
}

static const char *udp_reject_reason_name(udp_reject_reason_t reason)
{
    switch (reason) {
    case UDP_REJECT_INACTIVE: return "inactive";
    case UDP_REJECT_SOURCE: return "source_ip";
    case UDP_REJECT_HEADER: return "header";
    case UDP_REJECT_SESSION: return "session";
    case UDP_REJECT_FRAGMENT: return "fragment";
    case UDP_REJECT_LENGTH: return "length";
    case UDP_REJECT_NONE: default: return "none";
    }
}

static void log_udp_summary(void)
{
    const int64_t now = esp_timer_get_time();
    if (now - s_last_udp_summary_us < 1000 * 1000) return;
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    const bool active = s_session.active && s_session.started && s_session.video_transport == VIDEO_TRANSPORT_UDP;
    const uint32_t received = s_session.udp_rx_packets;
    const uint32_t accepted = s_session.udp_accepted_fragments;
    const uint32_t rejected = s_session.udp_rejected_packets;
    const uint32_t completed = s_session.udp_completed_frames;
    const uint32_t submitted = s_session.udp_submitted_frames;
    const uint32_t timeouts = s_session.udp_reassembly_timeouts;
    const uint32_t failed = s_session.udp_submit_failures;
    const char *last_reject = udp_reject_reason_name(s_session.udp_last_reject);
    xSemaphoreGive(s_session_lock);
    if (active) {
        ESP_LOGI(TAG, "UDP stats: rx=%lu accepted=%lu rejected=%lu completed=%lu submitted=%lu timeout=%lu failed=%lu last=%s",
                 (unsigned long)received, (unsigned long)accepted, (unsigned long)rejected,
                 (unsigned long)completed, (unsigned long)submitted, (unsigned long)timeouts,
                 (unsigned long)failed, last_reject);
    }
    s_last_udp_summary_us = now;
}

static subscreen_transport_status_t submit_result_status(frame_pipeline_submit_result_t result)
{
    switch (result) {
    case FRAME_PIPELINE_SUBMIT_BAD_HEADER: return SUBSCREEN_TRANSPORT_BAD_HEADER;
    case FRAME_PIPELINE_SUBMIT_BAD_CRC: return SUBSCREEN_TRANSPORT_BAD_CRC;
    case FRAME_PIPELINE_SUBMIT_NO_BUFFER: return SUBSCREEN_TRANSPORT_NO_BUFFER;
    case FRAME_PIPELINE_SUBMIT_BUSY: return SUBSCREEN_TRANSPORT_DEVICE_BUSY;
    case FRAME_PIPELINE_SUBMIT_INVALID_ARG: return SUBSCREEN_TRANSPORT_FRAME_REJECTED;
    case FRAME_PIPELINE_SUBMIT_OK: default: return SUBSCREEN_TRANSPORT_RECEIVING_FRAME;
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
    if (nvs_get_string("token", s_token, sizeof(s_token)) == ESP_OK && strlen(s_token) == 6) {
        bool numeric = true;
        for (size_t i = 0; i < 6; ++i) numeric = numeric && s_token[i] >= '0' && s_token[i] <= '9';
        if (numeric) return;
    }
    snprintf(s_token, sizeof(s_token), "%06lu", 100000UL + (esp_random() % 900000UL));
    nvs_set_string("token", s_token);
}

static bool websocket_authorized(httpd_req_t *req)
{
    char query[96] = {};
    char token[40] = {};
    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, "token", token, sizeof(token)) == ESP_OK && strcmp(token, s_token) == 0;
}

static esp_err_t send_control(httpd_req_t *req, const char *message)
{
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)message, .len = strlen(message)};
    return httpd_ws_send_frame(req, &frame);
}

static void reset_session_locked(void)
{
    if (s_session.active) {
        ESP_LOGI(TAG, "session release: fd=%d transport=%s started=%d rx=%lu submitted=%lu",
                 s_session.ws_fd, video_transport_name(s_session.video_transport), s_session.started,
                 (unsigned long)s_session.udp_rx_packets, (unsigned long)s_session.udp_submitted_frames);
    }
    if (s_session.started) frame_pipeline_release(pipeline_transport(s_session.video_transport));
    memset(&s_session, 0, sizeof(s_session));
}

static void reset_session_for_fd(int ws_fd)
{
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (s_session.active && s_session.ws_fd == ws_fd) reset_session_locked();
    xSemaphoreGive(s_session_lock);
    update_transport_status(SUBSCREEN_TRANSPORT_READY);
}

static void json_error(httpd_req_t *req, const char *request_id, const char *code)
{
    char response[160];
    snprintf(response, sizeof(response), "{\"type\":\"error\",\"request_id\":\"%s\",\"code\":\"%s\"}", request_id != NULL ? request_id : "", code);
    send_control(req, response);
    ESP_LOGW(TAG, "control rejected: request=%s code=%s", request_id != NULL ? request_id : "", code);
}

static bool json_get_string(const char *json, const char *name, char *value, size_t value_size)
{
    char key[40];
    snprintf(key, sizeof(key), "\"%s\"", name);
    const char *item = strstr(json, key);
    if (item == NULL || (item = strchr(item + strlen(key), ':')) == NULL) return false;
    while (*++item == ' ' || *item == '\t') {}
    if (*item++ != '\"') return false;
    const char *end = strchr(item, '\"');
    if (end == NULL || (size_t)(end - item) >= value_size) return false;
    memcpy(value, item, (size_t)(end - item));
    value[end - item] = '\0';
    return true;
}

static bool json_get_int(const char *json, const char *name, int *value)
{
    char key[40];
    snprintf(key, sizeof(key), "\"%s\"", name);
    const char *item = strstr(json, key);
    if (item == NULL || (item = strchr(item + strlen(key), ':')) == NULL) return false;
    return sscanf(item + 1, "%d", value) == 1;
}

static bool peer_address(httpd_req_t *req, struct in_addr *address)
{
    struct sockaddr_in peer = {};
    socklen_t length = sizeof(peer);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&peer, &length) != 0 ||
        peer.sin_family != AF_INET || peer.sin_addr.s_addr == 0) {
        return false;
    }
    *address = peer.sin_addr;
    return true;
}

static void handle_hello(httpd_req_t *req, const char *message, const char *request_id)
{
    int version = 0;
    char desired[8] = {};
    if (!json_get_int(message, "protocol_version", &version) || version != CONTROL_PROTOCOL_VERSION || !json_get_string(message, "video_transport", desired, sizeof(desired)) ||
        (strcmp(desired, "tcp") != 0 && strcmp(desired, "udp") != 0)) {
        json_error(req, request_id, "unsupported_protocol");
        return;
    }
    struct in_addr peer_addr = {};
    const bool peer_addr_valid = peer_address(req, &peer_addr);
    const int ws_fd = httpd_req_to_sockfd(req);
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (s_session.active && s_session.ws_fd != ws_fd) {
        xSemaphoreGive(s_session_lock);
        json_error(req, request_id, "device_busy");
        return;
    }
    s_session.active = true;
    s_session.negotiated = true;
    s_session.ws_fd = ws_fd;
    s_session.video_transport = strcmp(desired, "udp") == 0 ? VIDEO_TRANSPORT_UDP : VIDEO_TRANSPORT_TCP;
    s_session.peer_addr = peer_addr;
    s_session.peer_addr_bound = peer_addr_valid;
    s_session.session_id = esp_random();
    s_session.session_token = esp_random();
    s_session.last_control_us = esp_timer_get_time();
    uint32_t session_id = s_session.session_id;
    uint32_t session_token = s_session.session_token;
    video_transport_t transport = s_session.video_transport;
    xSemaphoreGive(s_session_lock);
    if (peer_addr_valid) {
        ESP_LOGI(TAG, "hello: peer=%s fd=%d transport=%s session=%08lx", inet_ntoa(peer_addr), ws_fd,
                 video_transport_name(transport), (unsigned long)session_id);
    } else {
        ESP_LOGW(TAG, "hello: fd=%d transport=%s has no usable TCP peer IPv4; waiting for authenticated UDP source",
                 ws_fd, video_transport_name(transport));
    }
    char response[420];
    snprintf(response, sizeof(response),
             "{\"type\":\"hello_ack\",\"request_id\":\"%s\",\"protocol_version\":1,\"video_transport\":\"%s\",\"width\":368,\"height\":448,\"max_frame_bytes\":%u,\"capabilities\":{\"video\":[\"tcp\",\"udp\"],\"touch\":true,\"audio_capture\":true,\"text_input\":true}}",
             request_id, video_transport_name(transport), (unsigned)(FRAME_LIMIT + sizeof(companion_frame_header_t)));
    if (transport == VIDEO_TRANSPORT_UDP) {
        snprintf(response, sizeof(response),
                 "{\"type\":\"hello_ack\",\"request_id\":\"%s\",\"protocol_version\":1,\"video_transport\":\"udp\",\"width\":368,\"height\":448,\"max_frame_bytes\":%u,\"udp\":{\"port\":%d,\"session_id\":%lu,\"session_token\":%lu,\"max_payload\":%u},\"capabilities\":{\"video\":[\"tcp\",\"udp\"],\"touch\":true,\"audio_capture\":true,\"text_input\":true}}",
                 request_id, (unsigned)(FRAME_LIMIT + sizeof(companion_frame_header_t)), CONFIG_COMPANION_UDP_PORT,
                 (unsigned long)session_id, (unsigned long)session_token, UDP_MAX_PAYLOAD);
    }
    update_transport_status(SUBSCREEN_TRANSPORT_NEGOTIATING);
    send_control(req, response);
}

static void handle_start(httpd_req_t *req, const char *request_id)
{
    const int ws_fd = httpd_req_to_sockfd(req);
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (!s_session.active || !s_session.negotiated || s_session.ws_fd != ws_fd) {
        xSemaphoreGive(s_session_lock); json_error(req, request_id, "not_negotiated"); return;
    }
    if (!s_session.started && !frame_pipeline_acquire(pipeline_transport(s_session.video_transport))) {
        xSemaphoreGive(s_session_lock); json_error(req, request_id, "device_busy"); return;
    }
    s_session.started = true;
    s_session.last_control_us = esp_timer_get_time();
    video_transport_t transport = s_session.video_transport;
    xSemaphoreGive(s_session_lock);
    ESP_LOGI(TAG, "stream started: fd=%d transport=%s", ws_fd, video_transport_name(transport));
    char response[160];
    snprintf(response, sizeof(response), "{\"type\":\"stream_started\",\"request_id\":\"%s\",\"video_transport\":\"%s\"}", request_id, video_transport_name(transport));
    update_transport_status(transport == VIDEO_TRANSPORT_UDP ? SUBSCREEN_TRANSPORT_UDP_VIDEO : SUBSCREEN_TRANSPORT_TCP_VIDEO);
    send_control(req, response);
}

static void handle_stop(httpd_req_t *req, const char *request_id)
{
    reset_session_for_fd(httpd_req_to_sockfd(req));
    char response[128];
    snprintf(response, sizeof(response), "{\"type\":\"stream_stopped\",\"request_id\":\"%s\"}", request_id);
    send_control(req, response);
}

static void handle_control(httpd_req_t *req, const uint8_t *payload, size_t length)
{
    (void)length;
    const char *message = (const char *)payload;
    char type[32] = {}, request_id[48] = {};
    if (!json_get_string(message, "type", type, sizeof(type)) || !json_get_string(message, "request_id", request_id, sizeof(request_id))) json_error(req, "", "invalid_control_message");
    else if (strcmp(type, "hello") == 0) handle_hello(req, message, request_id);
    else if (strcmp(type, "start_stream") == 0) handle_start(req, request_id);
    else if (strcmp(type, "stop_stream") == 0) handle_stop(req, request_id);
    else if (strcmp(type, "ping") == 0) {
        char response[128];
        snprintf(response, sizeof(response), "{\"type\":\"pong\",\"request_id\":\"%s\"}", request_id);
        send_control(req, response);
    } else json_error(req, request_id, "unsupported_message");
}

static void handle_tcp_frame(const uint8_t *data, size_t length)
{
    if (length < sizeof(companion_frame_header_t)) { update_transport_status(SUBSCREEN_TRANSPORT_BAD_SIZE); return; }
    const companion_frame_header_t *header = (const companion_frame_header_t *)data;
    if (length != sizeof(*header) + header->payload_size) { update_transport_status(SUBSCREEN_TRANSPORT_BAD_SIZE); return; }
    frame_pipeline_submit_result_t result = frame_pipeline_submit(COMPANION_TRANSPORT_TCP_VIDEO, header, data + sizeof(*header));
    update_transport_status(submit_result_status(result));
    if (result != FRAME_PIPELINE_SUBMIT_OK) ESP_LOGW(TAG, "TCP frame rejected: %s", frame_pipeline_submit_result_name(result));
}

static esp_err_t stream_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        if (!websocket_authorized(req)) { ESP_LOGW(TAG, "WebSocket rejected: invalid pairing code"); update_transport_status(SUBSCREEN_TRANSPORT_BAD_PAIRING_CODE); return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "pairing code required"); }
        xSemaphoreTake(s_session_lock, portMAX_DELAY);
        bool busy = s_session.active && s_session.ws_fd != httpd_req_to_sockfd(req);
        xSemaphoreGive(s_session_lock);
        if (busy) { ESP_LOGW(TAG, "WebSocket rejected: active session is busy"); update_transport_status(SUBSCREEN_TRANSPORT_DEVICE_BUSY); return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "control session active"); }
        ESP_LOGI(TAG, "WebSocket connected: fd=%d", httpd_req_to_sockfd(req));
        update_transport_status(SUBSCREEN_TRANSPORT_WS_CONNECTED);
        return ESP_OK;
    }
    httpd_ws_frame_t frame = {};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) return err;
    if (frame.type == HTTPD_WS_TYPE_CLOSE) { ESP_LOGI(TAG, "WebSocket close: fd=%d", httpd_req_to_sockfd(req)); reset_session_for_fd(httpd_req_to_sockfd(req)); return ESP_OK; }
    if (frame.type == HTTPD_WS_TYPE_PING && frame.len == 0) {
        httpd_ws_frame_t pong = {.type = HTTPD_WS_TYPE_PONG};
        return httpd_ws_send_frame(req, &pong);
    }
    if (frame.type == HTTPD_WS_TYPE_PONG && frame.len == 0) return ESP_OK;
    if (frame.len > FRAME_LIMIT + sizeof(companion_frame_header_t) || frame.len == 0) return ESP_ERR_INVALID_SIZE;
    uint8_t *data = malloc(frame.len + (frame.type == HTTPD_WS_TYPE_TEXT ? 1 : 0));
    if (data == NULL) return ESP_ERR_NO_MEM;
    frame.payload = data;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err == ESP_OK) {
        if (frame.type == HTTPD_WS_TYPE_TEXT) {
            data[frame.len] = '\0';
            handle_control(req, data, frame.len);
        }
        else if (frame.type == HTTPD_WS_TYPE_PONG) {
            /* ESP-IDF dispatches PONG frames to the URI handler; they are heartbeat traffic. */
        }
        else if (frame.type == HTTPD_WS_TYPE_PING) {
            httpd_ws_frame_t pong = {.type = HTTPD_WS_TYPE_PONG, .payload = data, .len = frame.len};
            err = httpd_ws_send_frame(req, &pong);
        }
        else if (frame.type == HTTPD_WS_TYPE_BINARY) {
            xSemaphoreTake(s_session_lock, portMAX_DELAY);
            bool accept = s_session.active && s_session.started && s_session.ws_fd == httpd_req_to_sockfd(req) && s_session.video_transport == VIDEO_TRANSPORT_TCP;
            xSemaphoreGive(s_session_lock);
            if (accept) handle_tcp_frame(data, frame.len); else json_error(req, "", "tcp_video_not_started");
        } else update_transport_status(SUBSCREEN_TRANSPORT_BAD_WS_FRAME);
    }
    free(data);
    return err;
}

static bool fragment_received(uint16_t index) { return (s_session.received_fragments[index / 64] & (1ULL << (index % 64))) != 0; }
static void mark_fragment_received(uint16_t index) { s_session.received_fragments[index / 64] |= 1ULL << (index % 64); }

static void reset_reassembly_locked(uint32_t sequence, uint32_t length, uint16_t fragments)
{
    s_session.reassembly_sequence = sequence;
    s_session.reassembly_length = length;
    s_session.reassembly_fragments = fragments;
    s_session.received_fragments[0] = 0;
    s_session.received_fragments[1] = 0;
    s_session.reassembly_started_us = esp_timer_get_time();
}

static bool reassembly_complete_locked(void)
{
    for (uint16_t i = 0; i < s_session.reassembly_fragments; ++i) if (!fragment_received(i)) return false;
    return true;
}

static void udp_task(void *arg)
{
    (void)arg;
    uint8_t packet[sizeof(companion_udp_header_t) + UDP_MAX_PAYLOAD];
    while (true) {
        struct sockaddr_in source = {};
        socklen_t source_length = sizeof(source);
        int received = recvfrom(s_udp_socket, packet, sizeof(packet), 0, (struct sockaddr *)&source, &source_length);
        int64_t now = esp_timer_get_time();
        if (received >= (int)sizeof(companion_udp_header_t)) {
            const companion_udp_header_t *header = (const companion_udp_header_t *)packet;
            size_t packet_payload = (size_t)received - sizeof(*header);
            xSemaphoreTake(s_session_lock, portMAX_DELAY);
            s_session.udp_rx_packets++;
            udp_reject_reason_t reject = UDP_REJECT_NONE;
            if (!s_session.active || !s_session.started || s_session.video_transport != VIDEO_TRANSPORT_UDP) reject = UDP_REJECT_INACTIVE;
            else if (header->magic != UDP_MAGIC || header->version != CONTROL_PROTOCOL_VERSION ||
                     header->stream_type != UDP_STREAM_VIDEO || header->header_size != sizeof(*header)) reject = UDP_REJECT_HEADER;
            else if (header->session_id != s_session.session_id || header->session_token != s_session.session_token) reject = UDP_REJECT_SESSION;
            else if (header->fragment_count == 0 || header->fragment_count > UDP_MAX_FRAGMENTS || header->fragment_index >= header->fragment_count) reject = UDP_REJECT_FRAGMENT;
            else if (header->payload_length != packet_payload || header->payload_length > UDP_MAX_PAYLOAD ||
                     header->frame_length < sizeof(companion_frame_header_t) || header->frame_length > FRAME_LIMIT + sizeof(companion_frame_header_t) ||
                     header->fragment_offset > header->frame_length || header->payload_length > header->frame_length - header->fragment_offset) reject = UDP_REJECT_LENGTH;
            else if (s_session.peer_addr_bound && source.sin_addr.s_addr != s_session.peer_addr.s_addr) reject = UDP_REJECT_SOURCE;
            if (reject == UDP_REJECT_NONE) {
                if (!s_session.peer_addr_bound) {
                    s_session.peer_addr = source.sin_addr;
                    s_session.peer_addr_bound = true;
                    ESP_LOGI(TAG, "UDP source bound: %s session=%08lx", inet_ntoa(source.sin_addr),
                             (unsigned long)s_session.session_id);
                }
                s_session.udp_accepted_fragments++;
                if (s_session.udp_accepted_fragments == 1) {
                    ESP_LOGI(TAG, "UDP first packet: frame=%lu fragments=%u bytes=%lu source=%s",
                             (unsigned long)header->frame_sequence, header->fragment_count,
                             (unsigned long)header->frame_length, inet_ntoa(source.sin_addr));
                }
                if (s_session.reassembly_sequence != header->frame_sequence || s_session.reassembly_length != header->frame_length ||
                    s_session.reassembly_fragments != header->fragment_count || now - s_session.reassembly_started_us > UDP_REASSEMBLY_TIMEOUT_US) {
                    if (s_session.reassembly_fragments != 0) {
                        s_session.dropped_frames++;
                        s_session.udp_reassembly_timeouts++;
                        ESP_LOGW(TAG, "UDP reassembly replaced: frame=%lu incomplete before frame=%lu",
                                 (unsigned long)s_session.reassembly_sequence, (unsigned long)header->frame_sequence);
                    }
                    reset_reassembly_locked(header->frame_sequence, header->frame_length, header->fragment_count);
                }
                if (!fragment_received(header->fragment_index)) {
                    memcpy(s_udp_reassembly + header->fragment_offset, packet + sizeof(*header), header->payload_length);
                    mark_fragment_received(header->fragment_index);
                }
                s_session.last_udp_us = now;
                if (reassembly_complete_locked()) {
                    uint32_t frame_length = s_session.reassembly_length;
                    s_session.reassembly_fragments = 0;
                    s_session.udp_completed_frames++;
                    xSemaphoreGive(s_session_lock);
                    const companion_frame_header_t *frame = (const companion_frame_header_t *)s_udp_reassembly;
                    if (frame_length == sizeof(*frame) + frame->payload_size) {
                        frame_pipeline_submit_result_t result = frame_pipeline_submit(COMPANION_TRANSPORT_UDP_VIDEO, frame, s_udp_reassembly + sizeof(*frame));
                        xSemaphoreTake(s_session_lock, portMAX_DELAY);
                        if (result == FRAME_PIPELINE_SUBMIT_OK) s_session.udp_submitted_frames++;
                        else s_session.udp_submit_failures++;
                        xSemaphoreGive(s_session_lock);
                        if (result != FRAME_PIPELINE_SUBMIT_OK) {
                            ESP_LOGW(TAG, "UDP frame rejected: frame=%lu result=%s", (unsigned long)header->frame_sequence,
                                     frame_pipeline_submit_result_name(result));
                        }
                        update_transport_status(submit_result_status(result));
                    } else update_transport_status(SUBSCREEN_TRANSPORT_BAD_SIZE);
                    continue;
                }
            } else {
                s_session.udp_rejected_packets++;
                s_session.udp_last_reject = reject;
                if (s_last_logged_reject != reject || now - s_last_udp_reject_log_us >= 1000 * 1000) {
                    char source_ip[16];
                    char expected_ip[16];
                    strlcpy(source_ip, inet_ntoa(source.sin_addr), sizeof(source_ip));
                    strlcpy(expected_ip, inet_ntoa(s_session.peer_addr), sizeof(expected_ip));
                    ESP_LOGW(TAG, "UDP rejected: reason=%s source=%s expected=%s frame=%lu session=%08lx len=%d",
                             udp_reject_reason_name(reject), source_ip, expected_ip,
                             (unsigned long)header->frame_sequence, (unsigned long)header->session_id, received);
                    s_last_logged_reject = reject;
                    s_last_udp_reject_log_us = now;
                }
                update_transport_status(SUBSCREEN_TRANSPORT_UDP_REJECTED);
            }
            xSemaphoreGive(s_session_lock);
        }
        xSemaphoreTake(s_session_lock, portMAX_DELAY);
        if (s_session.active && s_session.started && s_session.video_transport == VIDEO_TRANSPORT_UDP &&
            s_session.last_udp_us > 0 && now - s_session.last_udp_us > UDP_SESSION_TIMEOUT_US) {
            reset_session_locked();
            update_transport_status(SUBSCREEN_TRANSPORT_SESSION_EXPIRED);
        } else if (s_session.reassembly_fragments != 0 && now - s_session.reassembly_started_us > UDP_REASSEMBLY_TIMEOUT_US) {
            s_session.reassembly_fragments = 0;
            s_session.dropped_frames++;
            s_session.udp_reassembly_timeouts++;
            ESP_LOGW(TAG, "UDP reassembly timeout: frame=%lu", (unsigned long)s_session.reassembly_sequence);
            update_transport_status(SUBSCREEN_TRANSPORT_UDP_DROPPED);
        }
        xSemaphoreGive(s_session_lock);
        log_udp_summary();
    }
}

static esp_err_t root_get(httpd_req_t *req)
{
    static const char page[] = "<!doctype html><title>Companion Display</title><h1>Companion Display</h1><form method=post action=/api/wifi><label>Wi-Fi SSID <input name=ssid required></label><br><label>Password <input name=password type=password></label><br><button>Connect</button></form>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get(httpd_req_t *req)
{
    char response[640];
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    const char *video = s_session.started ? video_transport_name(s_session.video_transport) : "none";
    uint32_t dropped = s_session.dropped_frames;
    uint32_t rx_packets = s_session.udp_rx_packets;
    uint32_t accepted_fragments = s_session.udp_accepted_fragments;
    uint32_t rejected_packets = s_session.udp_rejected_packets;
    uint32_t completed_frames = s_session.udp_completed_frames;
    uint32_t submitted_frames = s_session.udp_submitted_frames;
    uint32_t submit_failures = s_session.udp_submit_failures;
    uint32_t reassembly_timeouts = s_session.udp_reassembly_timeouts;
    const char *last_reject = udp_reject_reason_name(s_session.udp_last_reject);
    xSemaphoreGive(s_session_lock);
    const char *wifi = s_network_status == SUBSCREEN_NETWORK_CONNECTED ? "connected" : s_network_status == SUBSCREEN_NETWORK_CONNECTING ? "connecting" : "setup_required";
    snprintf(response, sizeof(response), "{\"name\":\"companion-display\",\"width\":368,\"height\":448,\"wifi\":\"%s\",\"ipv4\":\"%s\",\"active_transport\":\"%s\",\"token_set\":true,\"protocol_version\":1,\"video_transports\":[\"tcp\",\"udp\"],\"touch\":true,\"audio_capture\":true,\"dropped_frames\":%lu,\"udp\":{\"rxPackets\":%lu,\"acceptedFragments\":%lu,\"rejectedPackets\":%lu,\"completedFrames\":%lu,\"submittedFrames\":%lu,\"submitFailures\":%lu,\"reassemblyTimeouts\":%lu,\"lastReject\":\"%s\"}}", wifi, s_ipv4, video, (unsigned long)dropped, (unsigned long)rx_packets, (unsigned long)accepted_fragments, (unsigned long)rejected_packets, (unsigned long)completed_frames, (unsigned long)submitted_frames, (unsigned long)submit_failures, (unsigned long)reassembly_timeouts, last_reject);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= 256) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request");
    char body[256] = {}, ssid[33] = {}, password[65] = {};
    if (httpd_req_recv(req, body, req->content_len) <= 0 || httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
    httpd_query_key_value(body, "password", password, sizeof(password));
    nvs_set_string("ssid", ssid); nvs_set_string("password", password);
    wifi_config_t config = {};
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid)); strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &config));
    s_has_credentials = true; update_network_status(SUBSCREEN_NETWORK_CONNECTING, NULL); ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    return httpd_resp_sendstr(req, "Connecting. You may close this page.");
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && s_has_credentials) { update_network_status(SUBSCREEN_NETWORK_CONNECTING, NULL); esp_wifi_connect(); }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) update_network_status(SUBSCREEN_NETWORK_SETUP_REQUIRED, NULL);
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && s_has_credentials) { update_network_status(SUBSCREEN_NETWORK_CONNECTING, NULL); esp_wifi_connect(); }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data; char ip[16]; snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip)); update_network_status(SUBSCREEN_NETWORK_CONNECTED, ip);
    }
}

static esp_err_t start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG(); config.server_port = CONFIG_COMPANION_HTTP_PORT; config.max_uri_handlers = 4; config.max_open_sockets = 4;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "HTTP server");
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    const httpd_uri_t status = {.uri = "/api/status", .method = HTTP_GET, .handler = status_get};
    const httpd_uri_t wifi = {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post};
    const httpd_uri_t stream = {.uri = "/stream", .method = HTTP_GET, .handler = stream_ws,
                                .is_websocket = true, .handle_ws_control_frames = true};
    httpd_register_uri_handler(s_server, &root); httpd_register_uri_handler(s_server, &status); httpd_register_uri_handler(s_server, &wifi); httpd_register_uri_handler(s_server, &stream);
    return ESP_OK;
}

static esp_err_t start_udp_server(void)
{
    s_udp_reassembly = heap_caps_malloc(FRAME_LIMIT + sizeof(companion_frame_header_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_udp_reassembly != NULL, ESP_ERR_NO_MEM, TAG, "UDP reassembly buffer");
    s_udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_RETURN_ON_FALSE(s_udp_socket >= 0, ESP_FAIL, TAG, "UDP socket");
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(CONFIG_COMPANION_UDP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    ESP_RETURN_ON_ERROR(bind(s_udp_socket, (struct sockaddr *)&address, sizeof(address)) == 0 ? ESP_OK : ESP_FAIL, TAG, "UDP bind");
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(s_udp_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ESP_RETURN_ON_FALSE(xTaskCreate(udp_task, "udp_video", 6144, NULL, 5, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "UDP task");
    return ESP_OK;
}

esp_err_t transport_wifi_init(subscreen_display_t *display)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); err = nvs_flash_init(); }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS");
    ensure_token(); s_display = display; s_session_lock = xSemaphoreCreateMutex(); ESP_RETURN_ON_FALSE(s_session_lock != NULL, ESP_ERR_NO_MEM, TAG, "session lock");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif"); ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_sta_netif = esp_netif_create_default_wifi_sta(); esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT(); ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL)); ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL));
    wifi_config_t ap = {}; uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP); snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "Companion-%02X%02X", mac[4], mac[5]); ap.ap.ssid_len = strlen((char *)ap.ap.ssid); strlcpy((char *)ap.ap.password, CONFIG_COMPANION_AP_PASSWORD, sizeof(ap.ap.password)); ap.ap.authmode = WIFI_AUTH_WPA2_PSK; ap.ap.max_connection = 2; ap.ap.channel = 1;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "wifi mode"); ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "AP config");
    char ssid[33] = {}, password[65] = {};
    if (nvs_get_string("ssid", ssid, sizeof(ssid)) == ESP_OK) { s_has_credentials = true; nvs_get_string("password", password, sizeof(password)); wifi_config_t sta = {}; strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid)); strlcpy((char *)sta.sta.password, password, sizeof(sta.sta.password)); esp_wifi_set_config(WIFI_IF_STA, &sta); } else update_network_status(SUBSCREEN_NETWORK_SETUP_REQUIRED, NULL);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start"); ESP_RETURN_ON_ERROR(start_server(), TAG, "HTTP start"); ESP_RETURN_ON_ERROR(start_udp_server(), TAG, "UDP start");
    ESP_LOGI(TAG, "control ready; UDP port=%d", CONFIG_COMPANION_UDP_PORT);
    return ESP_OK;
}
