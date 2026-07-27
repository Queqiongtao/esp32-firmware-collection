#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "frame_pipeline.h"
#include "touch_input.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COMPANION_CONTROL_PROTOCOL_VERSION 3
/* Advertised panel size comes from the per-project sdkconfig so the shared
 * protocol code serves boards with different displays. */
#define COMPANION_CONTROL_WIDTH CONFIG_COMPANION_DISPLAY_WIDTH
#define COMPANION_CONTROL_HEIGHT CONFIG_COMPANION_DISPLAY_HEIGHT
#define COMPANION_CONTROL_TOUCH_MAX_MOVE_HZ 60
#define COMPANION_CONTROL_TRANSPORT_TCP "tcp"
#define COMPANION_CONTROL_TRANSPORT_UDP "udp"

typedef struct {
    char type[32];
    char request_id[48];
    int protocol_version;
    char video_transport[8];
    bool touch_subscribe;
} companion_control_message_t;

typedef struct {
    const char *wifi;
    const char *ipv4;
    const char *active_transport;
    bool touch_hw;
    bool touch_events_active;
    uint32_t dropped_frames;
    frame_pipeline_presented_frame_t performance;
    uint32_t udp_rx_packets;
    uint32_t udp_accepted_fragments;
    uint32_t udp_rejected_packets;
    uint32_t udp_completed_frames;
    uint32_t udp_submitted_frames;
    uint32_t udp_submit_failures;
    uint32_t udp_reassembly_timeouts;
    const char *udp_last_reject;
} companion_status_snapshot_t;

bool companion_control_parse(const char *json, size_t length,
                             companion_control_message_t *message);
bool companion_control_transport_supported(const char *transport);

int companion_control_build_error(char *buffer, size_t buffer_size,
                                  const char *request_id, const char *code);
int companion_control_build_hello_ack(char *buffer, size_t buffer_size,
                                      const char *request_id,
                                      const char *video_transport,
                                      size_t max_frame_bytes,
                                      uint16_t udp_port,
                                      uint32_t session_id,
                                      uint32_t session_token,
                                      uint16_t udp_max_payload,
                                      bool touch_enabled);
int companion_control_build_stream_started(char *buffer, size_t buffer_size,
                                           const char *request_id,
                                           const char *video_transport);
int companion_control_build_stream_stopped(char *buffer, size_t buffer_size,
                                           const char *request_id);
int companion_control_build_stream_timeout(char *buffer, size_t buffer_size);
int companion_control_build_pong(char *buffer, size_t buffer_size,
                                 const char *request_id);
int companion_control_build_frame_presented(char *buffer, size_t buffer_size,
                                            const frame_pipeline_presented_frame_t *frame);
int companion_control_build_touch_events(char *buffer, size_t buffer_size,
                                         uint32_t seq,
                                         const touch_input_event_t *events,
                                         size_t count);
int companion_control_build_status(char *buffer, size_t buffer_size,
                                   const companion_status_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
