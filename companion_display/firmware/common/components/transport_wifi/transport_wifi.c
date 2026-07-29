#include "transport_wifi.h"

#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "companion_control.h"
#include "companion_udp.h"
#include "frame_pipeline.h"
#include "touch_input.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "subscreen_display.h"

#define UDP_REASSEMBLY_TIMEOUT_US (250 * 1000)
#define UDP_SESSION_TIMEOUT_US (5 * 1000 * 1000)
#define FRAME_TIMEOUT_US ((int64_t)CONFIG_COMPANION_FRAME_TIMEOUT_MS * 1000)
#define FRAME_LIMIT ((size_t)CONFIG_COMPANION_FRAME_LIMIT_KB * 1024U)
#define COMPLETE_FRAME_LIMIT (FRAME_LIMIT + sizeof(companion_frame_header_t))
#define UDP_FRAME_LIMIT \
    ((COMPLETE_FRAME_LIMIT < COMPANION_UDP_FRAGMENTED_FRAME_LIMIT) \
         ? COMPLETE_FRAME_LIMIT \
         : COMPANION_UDP_FRAGMENTED_FRAME_LIMIT)

typedef enum { VIDEO_TRANSPORT_NONE, VIDEO_TRANSPORT_TCP, VIDEO_TRANSPORT_UDP } video_transport_t;

typedef struct {
    bool active;
    bool negotiated;
    bool started;
    bool touch_enabled;
    uint32_t touch_seq;
    uint32_t generation;
    int ws_fd;
    video_transport_t video_transport;
    uint32_t session_id;
    uint32_t session_token;
    struct in_addr peer_addr;
    bool peer_addr_bound;
    int64_t last_control_us;
    int64_t last_udp_us;
    int64_t last_frame_us;
    bool udp_media_paused;
    uint32_t dropped_frames;
    uint32_t udp_rx_packets;
    uint32_t udp_accepted_fragments;
    uint32_t udp_rejected_packets;
    uint32_t udp_completed_frames;
    uint32_t udp_submitted_frames;
    uint32_t udp_submit_failures;
    uint32_t udp_reassembly_timeouts;
    uint32_t udp_fec_packets;
    uint32_t udp_fec_recovered_fragments;
    uint32_t udp_fec_unrecoverable_frames;
    uint32_t udp_latest_sequence;
    companion_udp_reject_reason_t udp_last_reject;
    const char *udp_last_outcome;
    uint32_t udp_last_outcome_sequence;
    uint32_t udp_last_reassembly_us;
    frame_pipeline_presented_frame_t last_presented;
    uint32_t reassembly_sequence;
    uint32_t reassembly_length;
    uint16_t reassembly_fragments;
    uint64_t received_fragments[2];
    uint8_t fec_group_received[COMPANION_UDP_MAX_FEC_GROUPS];
    uint16_t fec_parity_received;
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
/* Serializes stream start/release and complete-frame submission. Always take
 * this mutex before s_session_lock when both are needed. */
static SemaphoreHandle_t s_stream_transition_lock;
static SemaphoreHandle_t s_session_lock;
static stream_session_t s_session;
static uint32_t s_next_session_generation;
static int s_last_ws_fd = -1;
static uint8_t *s_udp_reassembly;
static uint8_t *s_udp_fec_accumulators;
static int64_t s_last_udp_summary_us;
static int64_t s_last_udp_reject_log_us;
static int64_t s_last_udp_feedback_us;
static int64_t s_last_control_heartbeat_log_us;
static companion_udp_reject_reason_t s_last_logged_reject;

typedef struct {
    int ws_fd;
    frame_pipeline_presented_frame_t frame;
} frame_feedback_work_t;

typedef struct {
    int ws_fd;
    uint32_t latest_sequence;
    uint32_t completed_frames;
    uint32_t submitted_frames;
    uint32_t recovered_fragments;
    uint32_t unrecoverable_frames;
    const char *outcome;
    uint32_t outcome_sequence;
    uint32_t reassembly_us;
} udp_feedback_work_t;

static udp_feedback_work_t s_udp_feedback_work[2];
static bool s_udp_feedback_pending[2];

typedef struct {
    int ws_fd;
    uint32_t seq;
    size_t count;
    touch_input_event_t events[TOUCH_INPUT_MAX_BATCH];
} touch_events_work_t;

typedef struct {
    bool released;
    bool started;
    video_transport_t transport;
} released_session_t;

typedef struct {
    int ws_fd;
    uint32_t generation;
    int64_t last_frame_us;
} frame_timeout_snapshot_t;

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

static void send_frame_feedback_work(void *arg)
{
    frame_feedback_work_t *work = arg;
    char message[256];
    companion_control_build_frame_presented(message, sizeof(message), &work->frame);
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)message, .len = strlen(message)};
    esp_err_t err = httpd_ws_send_frame_async(s_server, work->ws_fd, &frame);
    if (err != ESP_OK) ESP_LOGW(TAG, "frame feedback failed: %s", esp_err_to_name(err));
    free(work);
}

static void frame_presented(const frame_pipeline_presented_frame_t *frame, void *context)
{
    (void)context;
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    const bool active = s_session.active && s_session.started;
    const bool tcp = active && s_session.video_transport == VIDEO_TRANSPORT_TCP;
    const int ws_fd = s_session.ws_fd;
    if (active) s_session.last_presented = *frame;
    xSemaphoreGive(s_session_lock);
    if (!tcp || s_server == NULL) return;
    frame_feedback_work_t *work = malloc(sizeof(*work));
    if (work == NULL) return;
    work->ws_fd = ws_fd;
    work->frame = *frame;
    if (httpd_queue_work(s_server, send_frame_feedback_work, work) != ESP_OK) free(work);
}

static void send_udp_feedback_work(void *arg)
{
    udp_feedback_work_t *work = arg;
    char message[256];
    companion_control_build_udp_feedback(
        message, sizeof(message), work->latest_sequence, work->completed_frames,
        work->submitted_frames, work->recovered_fragments, work->unrecoverable_frames,
        work->outcome, work->outcome_sequence, work->reassembly_us);
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)message,
        .len = strlen(message),
    };
    esp_err_t err = httpd_ws_send_frame_async(s_server, work->ws_fd, &frame);
    if (err != ESP_OK) ESP_LOGW(TAG, "UDP feedback failed: %s", esp_err_to_name(err));
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    size_t slot = (size_t)(work - s_udp_feedback_work);
    if (slot < 2) s_udp_feedback_pending[slot] = false;
    xSemaphoreGive(s_session_lock);
}

static void queue_udp_feedback_locked(int64_t now, const char *outcome,
                                      uint32_t outcome_sequence, uint32_t reassembly_us)
{
    const bool immediate = outcome != NULL;
    if (s_server == NULL || (!immediate && now - s_last_udp_feedback_us < 500 * 1000)) return;
    if (immediate) {
        s_session.udp_last_outcome = outcome;
        s_session.udp_last_outcome_sequence = outcome_sequence;
        s_session.udp_last_reassembly_us = reassembly_us;
    }
    size_t slot = 0;
    while (slot < 2 && s_udp_feedback_pending[slot]) slot++;
    if (slot == 2) return;
    s_udp_feedback_work[slot] = (udp_feedback_work_t) {
        .ws_fd = s_session.ws_fd,
        .latest_sequence = s_session.udp_latest_sequence,
        .completed_frames = s_session.udp_completed_frames,
        .submitted_frames = s_session.udp_submitted_frames,
        .recovered_fragments = s_session.udp_fec_recovered_fragments,
        .unrecoverable_frames = s_session.udp_fec_unrecoverable_frames,
        .outcome = outcome,
        .outcome_sequence = outcome_sequence,
        .reassembly_us = reassembly_us,
    };
    s_udp_feedback_pending[slot] = true;
    if (httpd_queue_work(s_server, send_udp_feedback_work, &s_udp_feedback_work[slot]) != ESP_OK) {
        s_udp_feedback_pending[slot] = false;
        return;
    }
    s_last_udp_feedback_us = now;
}

static void send_touch_events_work(void *arg)
{
    touch_events_work_t *work = arg;
    char message[512];
    int written = companion_control_build_touch_events(message, sizeof(message), work->seq,
                                                       work->events, work->count);
    if (written > 0) {
        httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)message, .len = (size_t)written};
        esp_err_t err = httpd_ws_send_frame_async(s_server, work->ws_fd, &frame);
        if (err != ESP_OK) ESP_LOGW(TAG, "touch events send failed: %s", esp_err_to_name(err));
    }
    free(work);
}

static void touch_events_ready(const touch_input_event_t *events, size_t count, void *context)
{
    (void)context;
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    const bool send = s_session.active && s_session.started && s_session.touch_enabled;
    const int ws_fd = s_session.ws_fd;
    const uint32_t seq = send ? s_session.touch_seq++ : 0;
    xSemaphoreGive(s_session_lock);
    if (!send || s_server == NULL) return;
    touch_events_work_t *work = malloc(sizeof(*work));
    if (work == NULL) return;
    work->ws_fd = ws_fd;
    work->seq = seq;
    work->count = count > TOUCH_INPUT_MAX_BATCH ? TOUCH_INPUT_MAX_BATCH : count;
    memcpy(work->events, events, work->count * sizeof(*events));
    if (httpd_queue_work(s_server, send_touch_events_work, work) != ESP_OK) {
        ESP_LOGW(TAG, "touch events queue failed: seq=%lu dropped", (unsigned long)seq);
        free(work);
    }
}

static companion_transport_t pipeline_transport(video_transport_t transport)
{
    return transport == VIDEO_TRANSPORT_UDP ? COMPANION_TRANSPORT_UDP_VIDEO : COMPANION_TRANSPORT_TCP_VIDEO;
}

static const char *video_transport_name(video_transport_t transport)
{
    return transport == VIDEO_TRANSPORT_UDP ? "udp" : transport == VIDEO_TRANSPORT_TCP ? "tcp" : "none";
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
    const char *last_reject = companion_udp_reject_reason_name(s_session.udp_last_reject);
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

static released_session_t detach_session_locked(void)
{
    released_session_t released = {
        .released = s_session.active,
        .started = s_session.started,
        .transport = s_session.video_transport,
    };
    if (s_session.active) {
        ESP_LOGI(TAG, "session release: fd=%d transport=%s started=%d rx=%lu submitted=%lu",
                 s_session.ws_fd, video_transport_name(s_session.video_transport), s_session.started,
                 (unsigned long)s_session.udp_rx_packets, (unsigned long)s_session.udp_submitted_frames);
    }
    memset(&s_session, 0, sizeof(s_session));
    return released;
}

static void release_pipeline(const released_session_t *released)
{
    if (released->released && released->started) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(frame_pipeline_release(pipeline_transport(released->transport)));
    }
}

static void reset_session_for_fd(int ws_fd)
{
    released_session_t released = {};
    xSemaphoreTake(s_stream_transition_lock, portMAX_DELAY);
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (s_session.active && s_session.ws_fd == ws_fd) released = detach_session_locked();
    xSemaphoreGive(s_session_lock);
    release_pipeline(&released);
    if (released.released && s_display != NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            subscreen_display_return_to_placeholder(s_display, SUBSCREEN_TRANSPORT_READY));
    }
    xSemaphoreGive(s_stream_transition_lock);
}

static void control_socket_closed(httpd_handle_t server, int sockfd)
{
    (void)server;
    released_session_t released = {};
    xSemaphoreTake(s_stream_transition_lock, portMAX_DELAY);
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (s_session.active && s_session.ws_fd == sockfd) released = detach_session_locked();
    xSemaphoreGive(s_session_lock);
    release_pipeline(&released);
    if (released.released) {
        ESP_LOGI(TAG, "control socket closed: fd=%d", sockfd);
        if (s_display != NULL) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                subscreen_display_return_to_placeholder(s_display, SUBSCREEN_TRANSPORT_READY));
        }
    } else if (sockfd == s_last_ws_fd) {
        update_transport_status(SUBSCREEN_TRANSPORT_READY);
    }
    if (sockfd == s_last_ws_fd) s_last_ws_fd = -1;
    xSemaphoreGive(s_stream_transition_lock);
    close(sockfd);
}

static void send_stream_timeout_work(void *arg)
{
    const int ws_fd = (int)(intptr_t)arg;
    char message[96];
    companion_control_build_stream_timeout(message, sizeof(message));
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)message, .len = strlen(message)};
    esp_err_t err = httpd_ws_send_frame_async(s_server, ws_fd, &frame);
    if (err != ESP_OK) ESP_LOGW(TAG, "stream timeout notify failed: %s", esp_err_to_name(err));
    httpd_sess_trigger_close(s_server, ws_fd);
}

static void end_session_on_frame_timeout(const frame_timeout_snapshot_t *snapshot)
{
    released_session_t released = {};
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_stream_transition_lock, portMAX_DELAY);
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (s_session.active && s_session.started &&
        s_session.ws_fd == snapshot->ws_fd &&
        s_session.generation == snapshot->generation &&
        s_session.last_frame_us == snapshot->last_frame_us &&
        now - s_session.last_frame_us > FRAME_TIMEOUT_US) {
        released = detach_session_locked();
    }
    xSemaphoreGive(s_session_lock);
    if (!released.released) {
        xSemaphoreGive(s_stream_transition_lock);
        return;
    }

    ESP_LOGW(TAG, "stream frame timeout: fd=%d no frame for %d ms; ending session",
             snapshot->ws_fd, CONFIG_COMPANION_FRAME_TIMEOUT_MS);
    release_pipeline(&released);
    if (snapshot->ws_fd == s_last_ws_fd) s_last_ws_fd = -1;
    if (s_display != NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            subscreen_display_return_to_placeholder(s_display, SUBSCREEN_TRANSPORT_STREAM_TIMEOUT));
    }
    xSemaphoreGive(s_stream_transition_lock);
    if (s_server == NULL ||
        httpd_queue_work(s_server, send_stream_timeout_work,
                         (void *)(intptr_t)snapshot->ws_fd) != ESP_OK) {
        if (s_server != NULL) httpd_sess_trigger_close(s_server, snapshot->ws_fd);
    }
}

static void json_error(httpd_req_t *req, const char *request_id, const char *code)
{
    char response[160];
    companion_control_build_error(response, sizeof(response), request_id, code);
    send_control(req, response);
    ESP_LOGW(TAG, "control rejected: request=%s code=%s", request_id != NULL ? request_id : "", code);
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

static void handle_hello(httpd_req_t *req, const companion_control_message_t *message)
{
    if (message->protocol_version != COMPANION_CONTROL_PROTOCOL_VERSION ||
        !companion_control_transport_supported(message->video_transport)) {
        json_error(req, message->request_id, "unsupported_protocol");
        return;
    }
    struct in_addr peer_addr = {};
    const bool peer_addr_valid = peer_address(req, &peer_addr);
    const int ws_fd = httpd_req_to_sockfd(req);
    released_session_t released = {};
    xSemaphoreTake(s_stream_transition_lock, portMAX_DELAY);
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (s_session.active && s_session.ws_fd != ws_fd) {
        xSemaphoreGive(s_session_lock);
        xSemaphoreGive(s_stream_transition_lock);
        json_error(req, message->request_id, "device_busy");
        return;
    }
    if (s_session.active) released = detach_session_locked();
    xSemaphoreGive(s_session_lock);
    release_pipeline(&released);

    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    memset(&s_session, 0, sizeof(s_session));
    s_next_session_generation++;
    if (s_next_session_generation == 0) s_next_session_generation++;
    s_session.active = true;
    s_session.negotiated = true;
    s_session.generation = s_next_session_generation;
    s_session.ws_fd = ws_fd;
    s_session.video_transport = strcmp(message->video_transport, COMPANION_CONTROL_TRANSPORT_UDP) == 0 ? VIDEO_TRANSPORT_UDP : VIDEO_TRANSPORT_TCP;
    s_session.peer_addr = peer_addr;
    s_session.peer_addr_bound = peer_addr_valid;
    s_session.session_id = esp_random();
    s_session.session_token = esp_random();
    s_session.touch_enabled = message->touch_subscribe && touch_input_hw_ready();
    s_session.last_control_us = esp_timer_get_time();
    s_last_udp_feedback_us = 0;
    uint32_t session_id = s_session.session_id;
    uint32_t session_token = s_session.session_token;
    video_transport_t transport = s_session.video_transport;
    const bool touch_enabled = s_session.touch_enabled;
    xSemaphoreGive(s_session_lock);
    xSemaphoreGive(s_stream_transition_lock);
    if (message->touch_subscribe && !touch_enabled) {
        ESP_LOGW(TAG, "touch subscription refused: touch hardware not ready");
    }
    if (peer_addr_valid) {
        ESP_LOGI(TAG, "hello: peer=%s fd=%d transport=%s session=%08lx", inet_ntoa(peer_addr), ws_fd,
                 video_transport_name(transport), (unsigned long)session_id);
    } else {
        ESP_LOGW(TAG, "hello: fd=%d transport=%s has no usable TCP peer IPv4; waiting for authenticated UDP source",
                 ws_fd, video_transport_name(transport));
    }
    char response[768];
    const size_t session_frame_limit = transport == VIDEO_TRANSPORT_UDP
        ? UDP_FRAME_LIMIT
        : COMPLETE_FRAME_LIMIT;
    companion_control_build_hello_ack(response, sizeof(response), message->request_id,
                                      video_transport_name(transport),
                                      session_frame_limit,
                                      CONFIG_COMPANION_UDP_PORT, session_id, session_token,
                                      COMPANION_UDP_MAX_PAYLOAD, touch_enabled);
    update_transport_status(SUBSCREEN_TRANSPORT_NEGOTIATING);
    send_control(req, response);
}

static void handle_start(httpd_req_t *req, const char *request_id)
{
    const int ws_fd = httpd_req_to_sockfd(req);
    xSemaphoreTake(s_stream_transition_lock, portMAX_DELAY);
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (!s_session.active || !s_session.negotiated || s_session.ws_fd != ws_fd) {
        xSemaphoreGive(s_session_lock);
        xSemaphoreGive(s_stream_transition_lock);
        json_error(req, request_id, "not_negotiated");
        return;
    }
    if (!s_session.started && !frame_pipeline_acquire(pipeline_transport(s_session.video_transport))) {
        xSemaphoreGive(s_session_lock);
        xSemaphoreGive(s_stream_transition_lock);
        json_error(req, request_id, "device_busy");
        return;
    }
    s_session.started = true;
    s_session.last_control_us = esp_timer_get_time();
    s_session.last_frame_us = s_session.last_control_us;
    video_transport_t transport = s_session.video_transport;
    xSemaphoreGive(s_session_lock);
    xSemaphoreGive(s_stream_transition_lock);
    ESP_LOGI(TAG, "stream started: fd=%d transport=%s", ws_fd, video_transport_name(transport));
    char response[160];
    companion_control_build_stream_started(response, sizeof(response), request_id, video_transport_name(transport));
    update_transport_status(transport == VIDEO_TRANSPORT_UDP ? SUBSCREEN_TRANSPORT_UDP_VIDEO : SUBSCREEN_TRANSPORT_TCP_VIDEO);
    send_control(req, response);
}

static void handle_stop(httpd_req_t *req, const char *request_id)
{
    reset_session_for_fd(httpd_req_to_sockfd(req));
    char response[128];
    companion_control_build_stream_stopped(response, sizeof(response), request_id);
    send_control(req, response);
}

static void handle_control(httpd_req_t *req, const uint8_t *payload, size_t length)
{
    const char *message = (const char *)payload;
    companion_control_message_t control = {};
    if (!companion_control_parse(message, length, &control)) {
        json_error(req, "", "invalid_control_message");
        return;
    }
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (s_session.active && s_session.ws_fd == httpd_req_to_sockfd(req)) s_session.last_control_us = esp_timer_get_time();
    xSemaphoreGive(s_session_lock);
    if (strcmp(control.type, "hello") == 0) handle_hello(req, &control);
    else if (strcmp(control.type, "start_stream") == 0) handle_start(req, control.request_id);
    else if (strcmp(control.type, "stop_stream") == 0) handle_stop(req, control.request_id);
    else if (strcmp(control.type, "ping") == 0) {
        char response[128];
        companion_control_build_pong(response, sizeof(response), control.request_id);
        const int64_t now = esp_timer_get_time();
        if (now - s_last_control_heartbeat_log_us >= 30 * 1000 * 1000) {
            ESP_LOGI(TAG, "control heartbeat: fd=%d", httpd_req_to_sockfd(req));
            s_last_control_heartbeat_log_us = now;
        }
        send_control(req, response);
    } else json_error(req, control.request_id, "unsupported_message");
}

static void handle_tcp_frame(int ws_fd, const uint8_t *data, size_t length)
{
    xSemaphoreTake(s_stream_transition_lock, portMAX_DELAY);
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    const bool accept = s_session.active && s_session.started && s_session.ws_fd == ws_fd &&
                        s_session.video_transport == VIDEO_TRANSPORT_TCP;
    const uint32_t generation = s_session.generation;
    xSemaphoreGive(s_session_lock);
    if (!accept) {
        xSemaphoreGive(s_stream_transition_lock);
        return;
    }
    if (length < sizeof(companion_frame_header_t)) {
        xSemaphoreGive(s_stream_transition_lock);
        update_transport_status(SUBSCREEN_TRANSPORT_BAD_SIZE);
        return;
    }
    const companion_frame_header_t *header = (const companion_frame_header_t *)data;
    if (length != sizeof(*header) + header->payload_size) {
        xSemaphoreGive(s_stream_transition_lock);
        update_transport_status(SUBSCREEN_TRANSPORT_BAD_SIZE);
        return;
    }
    frame_pipeline_submit_result_t result =
        frame_pipeline_submit(COMPANION_TRANSPORT_TCP_VIDEO, header, data + sizeof(*header));
    if (result == FRAME_PIPELINE_SUBMIT_OK) {
        xSemaphoreTake(s_session_lock, portMAX_DELAY);
        if (s_session.active && s_session.started && s_session.ws_fd == ws_fd &&
            s_session.generation == generation) {
            s_session.last_frame_us = esp_timer_get_time();
        }
        xSemaphoreGive(s_session_lock);
    }
    xSemaphoreGive(s_stream_transition_lock);
    update_transport_status(submit_result_status(result));
    if (result != FRAME_PIPELINE_SUBMIT_OK) ESP_LOGW(TAG, "TCP frame rejected: %s", frame_pipeline_submit_result_name(result));
}

static esp_err_t stream_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        if (!websocket_authorized(req)) { ESP_LOGW(TAG, "WebSocket rejected: invalid pairing code"); update_transport_status(SUBSCREEN_TRANSPORT_BAD_PAIRING_CODE); return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "pairing code required"); }
        const int ws_fd = httpd_req_to_sockfd(req);
        struct in_addr peer_addr = {};
        const bool peer_addr_valid = peer_address(req, &peer_addr);
        released_session_t released = {};
        xSemaphoreTake(s_stream_transition_lock, portMAX_DELAY);
        xSemaphoreTake(s_session_lock, portMAX_DELAY);
        bool busy = s_session.active && s_session.ws_fd != ws_fd;
        const bool replacing_same_peer = busy && peer_addr_valid && s_session.peer_addr_bound &&
                                         s_session.peer_addr.s_addr == peer_addr.s_addr;
        if (replacing_same_peer) {
            ESP_LOGW(TAG, "WebSocket reconnect replacing stale session: old_fd=%d new_fd=%d peer=%s",
                     s_session.ws_fd, ws_fd, inet_ntoa(peer_addr));
            released = detach_session_locked();
            busy = false;
        }
        xSemaphoreGive(s_session_lock);
        release_pipeline(&released);
        if (busy) {
            xSemaphoreGive(s_stream_transition_lock);
            ESP_LOGW(TAG, "WebSocket rejected: active session is busy");
            update_transport_status(SUBSCREEN_TRANSPORT_DEVICE_BUSY);
            return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "control session active");
        }
        ESP_LOGI(TAG, "WebSocket connected: fd=%d", ws_fd);
        s_last_ws_fd = ws_fd;
        update_transport_status(SUBSCREEN_TRANSPORT_WS_CONNECTED);
        xSemaphoreGive(s_stream_transition_lock);
        return ESP_OK;
    }
    httpd_ws_frame_t frame = {};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) return err;
    if (frame.type == HTTPD_WS_TYPE_CLOSE) { ESP_LOGI(TAG, "WebSocket peer closed: fd=%d", httpd_req_to_sockfd(req)); reset_session_for_fd(httpd_req_to_sockfd(req)); return ESP_OK; }
    if (frame.type == HTTPD_WS_TYPE_PING && frame.len == 0) {
        httpd_ws_frame_t pong = {.type = HTTPD_WS_TYPE_PONG};
        return httpd_ws_send_frame(req, &pong);
    }
    if (frame.type == HTTPD_WS_TYPE_PONG && frame.len == 0) return ESP_OK;
    if (frame.len > COMPLETE_FRAME_LIMIT || frame.len == 0) return ESP_ERR_INVALID_SIZE;
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
            if (accept) handle_tcp_frame(httpd_req_to_sockfd(req), data, frame.len);
            else json_error(req, "", "tcp_video_not_started");
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
    memset(s_session.fec_group_received, 0, sizeof(s_session.fec_group_received));
    s_session.fec_parity_received = 0;
    memset(s_udp_fec_accumulators, 0,
           COMPANION_UDP_MAX_FEC_GROUPS * COMPANION_UDP_MAX_PAYLOAD);
    s_session.reassembly_started_us = esp_timer_get_time();
}

static uint8_t *fec_accumulator(uint16_t group)
{
    return s_udp_fec_accumulators + (size_t)group * COMPANION_UDP_MAX_PAYLOAD;
}

static void xor_fec_payload(uint16_t group, const uint8_t *payload, size_t length)
{
    uint8_t *accumulator = fec_accumulator(group);
    for (size_t i = 0; i < length; ++i) accumulator[i] ^= payload[i];
}

static void recover_fec_fragments_locked(void)
{
    const uint16_t fragments = s_session.reassembly_fragments;
    const uint16_t groups = companion_udp_fec_group_count(fragments);
    for (uint16_t group = 0; group < groups; ++group) {
        if ((s_session.fec_parity_received & (1U << group)) == 0) continue;
        const uint16_t expected = (fragments + groups - 1U - group) / groups;
        if (s_session.fec_group_received[group] + 1U != expected) continue;
        uint16_t missing = fragments;
        for (uint16_t index = group; index < fragments; index += groups) {
            if (!fragment_received(index)) {
                if (missing != fragments) {
                    missing = fragments;
                    break;
                }
                missing = index;
            }
        }
        if (missing == fragments) continue;
        const size_t offset = (size_t)missing * COMPANION_UDP_MAX_PAYLOAD;
        const size_t remaining = s_session.reassembly_length - offset;
        const size_t length = remaining < COMPANION_UDP_MAX_PAYLOAD ? remaining : COMPANION_UDP_MAX_PAYLOAD;
        memcpy(s_udp_reassembly + offset, fec_accumulator(group), length);
        mark_fragment_received(missing);
        s_session.fec_group_received[group]++;
        s_session.udp_fec_recovered_fragments++;
    }
}

static bool reassembly_complete_locked(void)
{
    for (uint16_t i = 0; i < s_session.reassembly_fragments; ++i) if (!fragment_received(i)) return false;
    return true;
}

static void udp_task(void *arg)
{
    (void)arg;
    uint8_t packet[sizeof(companion_udp_header_t) + COMPANION_UDP_MAX_PAYLOAD];
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
            const bool session_active = s_session.active && s_session.started &&
                                        s_session.video_transport == VIDEO_TRANSPORT_UDP;
            companion_udp_reject_reason_t reject = companion_udp_validate_packet(
                header, packet_payload, UDP_FRAME_LIMIT,
                session_active, s_session.session_id, s_session.session_token,
                s_session.peer_addr_bound, s_session.peer_addr.s_addr,
                source.sin_addr.s_addr);
            if (reject == COMPANION_UDP_REJECT_NONE) {
                if (s_session.udp_media_paused) {
                    s_session.udp_media_paused = false;
                    ESP_LOGI(TAG, "UDP resumed: session=%08lx frame=%lu", (unsigned long)s_session.session_id,
                             (unsigned long)header->frame_sequence);
                }
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
                        const uint32_t replaced_sequence = s_session.reassembly_sequence;
                        const uint32_t reassembly_us = (uint32_t)(now - s_session.reassembly_started_us);
                        s_session.dropped_frames++;
                        s_session.udp_reassembly_timeouts++;
                        s_session.udp_fec_unrecoverable_frames++;
                        s_session.udp_latest_sequence = s_session.reassembly_sequence;
                        ESP_LOGW(TAG, "UDP reassembly replaced: frame=%lu incomplete before frame=%lu",
                                 (unsigned long)s_session.reassembly_sequence, (unsigned long)header->frame_sequence);
                        queue_udp_feedback_locked(now, "unrecoverable", replaced_sequence, reassembly_us);
                    }
                    reset_reassembly_locked(header->frame_sequence, header->frame_length, header->fragment_count);
                }
                const bool fec = companion_udp_is_fec(header);
                const uint16_t groups = companion_udp_fec_group_count(header->fragment_count);
                if (fec) {
                    const uint16_t group = header->fragment_index;
                    if ((s_session.fec_parity_received & (1U << group)) == 0) {
                        xor_fec_payload(group, packet + sizeof(*header), header->payload_length);
                        s_session.fec_parity_received |= 1U << group;
                        s_session.udp_fec_packets++;
                    }
                } else if (!fragment_received(header->fragment_index)) {
                    memcpy(s_udp_reassembly + header->fragment_offset, packet + sizeof(*header), header->payload_length);
                    mark_fragment_received(header->fragment_index);
                    const uint16_t group = header->fragment_index % groups;
                    xor_fec_payload(group, packet + sizeof(*header), header->payload_length);
                    s_session.fec_group_received[group]++;
                }
                recover_fec_fragments_locked();
                s_session.last_udp_us = now;
                if (reassembly_complete_locked()) {
                    uint32_t frame_length = s_session.reassembly_length;
                    const uint32_t reassembly_us = (uint32_t)(now - s_session.reassembly_started_us);
                    const uint32_t session_generation = s_session.generation;
                    s_session.reassembly_fragments = 0;
                    s_session.udp_completed_frames++;
                    s_session.udp_latest_sequence = header->frame_sequence;
                    xSemaphoreGive(s_session_lock);
                    const companion_frame_header_t *frame = (const companion_frame_header_t *)s_udp_reassembly;
                    if (frame_length == sizeof(*frame) + frame->payload_size) {
                        xSemaphoreTake(s_stream_transition_lock, portMAX_DELAY);
                        xSemaphoreTake(s_session_lock, portMAX_DELAY);
                        const bool accept = s_session.active && s_session.started &&
                                            s_session.generation == session_generation &&
                                            s_session.video_transport == VIDEO_TRANSPORT_UDP;
                        xSemaphoreGive(s_session_lock);
                        frame_pipeline_submit_result_t result = accept ?
                            frame_pipeline_submit(COMPANION_TRANSPORT_UDP_VIDEO, frame,
                                                  s_udp_reassembly + sizeof(*frame)) :
                            FRAME_PIPELINE_SUBMIT_BUSY;
                        xSemaphoreTake(s_session_lock, portMAX_DELAY);
                        if (s_session.active && s_session.started &&
                            s_session.generation == session_generation) {
                            if (result == FRAME_PIPELINE_SUBMIT_OK) {
                                s_session.udp_submitted_frames++;
                                s_session.last_frame_us = esp_timer_get_time();
                            } else {
                                s_session.udp_submit_failures++;
                            }
                            queue_udp_feedback_locked(esp_timer_get_time(),
                                result == FRAME_PIPELINE_SUBMIT_OK ? "submitted" : "submit_failed",
                                header->frame_sequence, reassembly_us);
                        }
                        xSemaphoreGive(s_session_lock);
                        xSemaphoreGive(s_stream_transition_lock);
                        if (accept && result != FRAME_PIPELINE_SUBMIT_OK) {
                            ESP_LOGW(TAG, "UDP frame rejected: frame=%lu result=%s", (unsigned long)header->frame_sequence,
                                     frame_pipeline_submit_result_name(result));
                        }
                        if (accept) update_transport_status(submit_result_status(result));
                    } else {
                        xSemaphoreTake(s_stream_transition_lock, portMAX_DELAY);
                        xSemaphoreTake(s_session_lock, portMAX_DELAY);
                        const bool current = s_session.active && s_session.started &&
                                             s_session.generation == session_generation &&
                                             s_session.video_transport == VIDEO_TRANSPORT_UDP;
                        if (current) {
                            s_session.udp_submit_failures++;
                            queue_udp_feedback_locked(esp_timer_get_time(), "submit_failed",
                                                      header->frame_sequence, reassembly_us);
                        }
                        xSemaphoreGive(s_session_lock);
                        xSemaphoreGive(s_stream_transition_lock);
                        if (current) update_transport_status(SUBSCREEN_TRANSPORT_BAD_SIZE);
                    }
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
                             companion_udp_reject_reason_name(reject), source_ip, expected_ip,
                             (unsigned long)header->frame_sequence, (unsigned long)header->session_id, received);
                    s_last_logged_reject = reject;
                    s_last_udp_reject_log_us = now;
                }
                if (session_active) update_transport_status(SUBSCREEN_TRANSPORT_UDP_REJECTED);
            }
            xSemaphoreGive(s_session_lock);
        }
        xSemaphoreTake(s_session_lock, portMAX_DELAY);
        frame_timeout_snapshot_t frame_timeout = {};
        if (s_session.active && s_session.started && s_session.last_frame_us > 0 &&
            now - s_session.last_frame_us > FRAME_TIMEOUT_US) {
            frame_timeout = (frame_timeout_snapshot_t) {
                .ws_fd = s_session.ws_fd,
                .generation = s_session.generation,
                .last_frame_us = s_session.last_frame_us,
            };
        }
        if (s_session.active && s_session.started && s_session.video_transport == VIDEO_TRANSPORT_UDP &&
            s_session.last_udp_us > 0 && now - s_session.last_udp_us > UDP_SESSION_TIMEOUT_US) {
            if (!s_session.udp_media_paused) {
                s_session.udp_media_paused = true;
                s_session.reassembly_fragments = 0;
                ESP_LOGW(TAG, "UDP media paused: no packets for %d ms; control session retained", (int)(UDP_SESSION_TIMEOUT_US / 1000));
                update_transport_status(SUBSCREEN_TRANSPORT_SESSION_EXPIRED);
            }
        } else if (s_session.reassembly_fragments != 0 && now - s_session.reassembly_started_us > UDP_REASSEMBLY_TIMEOUT_US) {
            const uint32_t timed_out_sequence = s_session.reassembly_sequence;
            const uint32_t reassembly_us = (uint32_t)(now - s_session.reassembly_started_us);
            s_session.reassembly_fragments = 0;
            s_session.dropped_frames++;
            s_session.udp_reassembly_timeouts++;
            s_session.udp_fec_unrecoverable_frames++;
            s_session.udp_latest_sequence = s_session.reassembly_sequence;
            ESP_LOGW(TAG, "UDP reassembly timeout: frame=%lu", (unsigned long)s_session.reassembly_sequence);
            update_transport_status(SUBSCREEN_TRANSPORT_UDP_DROPPED);
            queue_udp_feedback_locked(now, "unrecoverable", timed_out_sequence, reassembly_us);
        }
        if (s_session.active && s_session.started && s_session.video_transport == VIDEO_TRANSPORT_UDP) {
            queue_udp_feedback_locked(now, NULL, 0, 0);
        }
        xSemaphoreGive(s_session_lock);
        if (frame_timeout.generation != 0) end_session_on_frame_timeout(&frame_timeout);
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
    char response[1152];
    companion_status_snapshot_t snapshot = {};
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    snapshot.active_transport = s_session.started ? video_transport_name(s_session.video_transport) : "none";
    snapshot.touch_events_active = s_session.started && s_session.touch_enabled;
    snapshot.dropped_frames = s_session.dropped_frames;
    snapshot.udp_rx_packets = s_session.udp_rx_packets;
    snapshot.udp_accepted_fragments = s_session.udp_accepted_fragments;
    snapshot.udp_rejected_packets = s_session.udp_rejected_packets;
    snapshot.udp_completed_frames = s_session.udp_completed_frames;
    snapshot.udp_submitted_frames = s_session.udp_submitted_frames;
    snapshot.udp_submit_failures = s_session.udp_submit_failures;
    snapshot.udp_reassembly_timeouts = s_session.udp_reassembly_timeouts;
    snapshot.udp_fec_packets = s_session.udp_fec_packets;
    snapshot.udp_fec_recovered_fragments = s_session.udp_fec_recovered_fragments;
    snapshot.udp_fec_unrecoverable_frames = s_session.udp_fec_unrecoverable_frames;
    snapshot.udp_last_reject = companion_udp_reject_reason_name(s_session.udp_last_reject);
    snapshot.udp_last_outcome = s_session.udp_last_outcome;
    snapshot.udp_last_outcome_sequence = s_session.udp_last_outcome_sequence;
    snapshot.udp_last_reassembly_us = s_session.udp_last_reassembly_us;
    snapshot.performance = s_session.last_presented;
    xSemaphoreGive(s_session_lock);
    snapshot.touch_hw = touch_input_hw_ready();
    snapshot.wifi = s_network_status == SUBSCREEN_NETWORK_CONNECTED ? "connected" : s_network_status == SUBSCREEN_NETWORK_CONNECTING ? "connecting" : "setup_required";
    snapshot.ipv4 = s_ipv4;
    companion_control_build_status(response, sizeof(response), &snapshot);
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
    config.close_fn = control_socket_closed;
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
    s_udp_reassembly = heap_caps_malloc(UDP_FRAME_LIMIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_udp_reassembly != NULL, ESP_ERR_NO_MEM, TAG, "UDP reassembly buffer");
    s_udp_fec_accumulators = heap_caps_malloc(
        COMPANION_UDP_MAX_FEC_GROUPS * COMPANION_UDP_MAX_PAYLOAD,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_udp_fec_accumulators != NULL, ESP_ERR_NO_MEM, TAG, "UDP FEC accumulators");
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
    ensure_token();
    s_display = display;
    s_stream_transition_lock = xSemaphoreCreateMutex();
    s_session_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_stream_transition_lock != NULL && s_session_lock != NULL,
                        ESP_ERR_NO_MEM, TAG, "stream synchronization locks");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    /* Boards with a network co-processor (ESP32-P4 + hosted) create the
     * default event loop before this init runs; tolerate that. */
    esp_err_t loop_err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(loop_err == ESP_OK || loop_err == ESP_ERR_INVALID_STATE, loop_err, TAG, "event loop");
    s_sta_netif = esp_netif_create_default_wifi_sta(); esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT(); ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL)); ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL));
    wifi_config_t ap = {}; uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_AP, mac), TAG, "read SoftAP MAC");
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "Companion-%02X%02X", mac[4], mac[5]); ap.ap.ssid_len = strlen((char *)ap.ap.ssid); strlcpy((char *)ap.ap.password, CONFIG_COMPANION_AP_PASSWORD, sizeof(ap.ap.password)); ap.ap.authmode = WIFI_AUTH_WPA2_PSK; ap.ap.max_connection = 2; ap.ap.channel = 1;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "wifi mode"); ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "AP config");
    char ssid[33] = {}, password[65] = {};
    if (nvs_get_string("ssid", ssid, sizeof(ssid)) == ESP_OK) { s_has_credentials = true; nvs_get_string("password", password, sizeof(password)); wifi_config_t sta = {}; strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid)); strlcpy((char *)sta.sta.password, password, sizeof(sta.sta.password)); esp_wifi_set_config(WIFI_IF_STA, &sta); } else update_network_status(SUBSCREEN_NETWORK_SETUP_REQUIRED, NULL);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start"); ESP_RETURN_ON_ERROR(start_server(), TAG, "HTTP start"); ESP_RETURN_ON_ERROR(start_udp_server(), TAG, "UDP start");
    frame_pipeline_set_presented_callback(frame_presented, NULL);
    ESP_ERROR_CHECK_WITHOUT_ABORT(touch_input_start(touch_events_ready, NULL));
    ESP_LOGI(TAG, "control ready; UDP port=%d", CONFIG_COMPANION_UDP_PORT);
    return ESP_OK;
}
