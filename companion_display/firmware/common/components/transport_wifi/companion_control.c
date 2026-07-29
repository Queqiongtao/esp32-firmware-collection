#include "companion_control.h"
#include "companion_udp.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void skip_ws(const char **cursor, const char *end)
{
    while (*cursor < end && (**cursor == ' ' || **cursor == '\t' ||
           **cursor == '\r' || **cursor == '\n')) {
        (*cursor)++;
    }
}

static bool parse_json_string(const char **cursor, const char *end,
                              char *value, size_t value_size)
{
    if (*cursor >= end || **cursor != '"' || value_size == 0) {
        return false;
    }
    (*cursor)++;
    size_t out = 0;
    while (*cursor < end && **cursor != '"') {
        if (**cursor == '\\') {
            return false;
        }
        if (out + 1 >= value_size) {
            return false;
        }
        value[out++] = **cursor;
        (*cursor)++;
    }
    if (*cursor >= end || **cursor != '"') {
        return false;
    }
    (*cursor)++;
    value[out] = '\0';
    return true;
}

static bool parse_json_int(const char **cursor, const char *end, int *value)
{
    char number[16] = {};
    size_t length = 0;
    if (*cursor < end && **cursor == '-') {
        number[length++] = **cursor;
        (*cursor)++;
    }
    while (*cursor < end && **cursor >= '0' && **cursor <= '9') {
        if (length + 1 >= sizeof(number)) {
            return false;
        }
        number[length++] = **cursor;
        (*cursor)++;
    }
    if (length == 0 || (length == 1 && number[0] == '-')) {
        return false;
    }
    *value = atoi(number);
    return true;
}

static bool parse_json_bool(const char **cursor, const char *end, bool *value)
{
    if (end - *cursor >= 4 && strncmp(*cursor, "true", 4) == 0) {
        *cursor += 4;
        *value = true;
        return true;
    }
    if (end - *cursor >= 5 && strncmp(*cursor, "false", 5) == 0) {
        *cursor += 5;
        *value = false;
        return true;
    }
    return false;
}

static bool skip_json_string(const char **cursor, const char *end)
{
    if (*cursor >= end || **cursor != '"') {
        return false;
    }
    (*cursor)++;
    while (*cursor < end) {
        if (**cursor == '\\') {
            (*cursor)++;
            if (*cursor >= end) {
                return false;
            }
            (*cursor)++;
            continue;
        }
        if (**cursor == '"') {
            (*cursor)++;
            return true;
        }
        (*cursor)++;
    }
    return false;
}

static bool skip_json_value(const char **cursor, const char *end)
{
    skip_ws(cursor, end);
    if (*cursor >= end) {
        return false;
    }
    if (**cursor == '"') {
        return skip_json_string(cursor, end);
    }
    if (**cursor == '{' || **cursor == '[') {
        char stack[8];
        size_t depth = 0;
        do {
            if (**cursor == '"') {
                if (!skip_json_string(cursor, end)) {
                    return false;
                }
                continue;
            }
            if (**cursor == '{' || **cursor == '[') {
                if (depth >= sizeof(stack)) {
                    return false;
                }
                stack[depth++] = **cursor == '{' ? '}' : ']';
            } else if (**cursor == '}' || **cursor == ']') {
                if (depth == 0 || **cursor != stack[depth - 1]) {
                    return false;
                }
                depth--;
            }
            (*cursor)++;
        } while (*cursor < end && depth > 0);
        return depth == 0;
    }
    while (*cursor < end && **cursor != ',' && **cursor != '}') {
        (*cursor)++;
    }
    return true;
}

static bool parse_features_object(const char **cursor, const char *end,
                                  companion_control_message_t *message)
{
    skip_ws(cursor, end);
    if (*cursor >= end || **cursor != '{') {
        return false;
    }
    (*cursor)++;
    while (true) {
        char key[24] = {};
        skip_ws(cursor, end);
        if (*cursor < end && **cursor == '}') {
            (*cursor)++;
            return true;
        }
        if (!parse_json_string(cursor, end, key, sizeof(key))) {
            return false;
        }
        skip_ws(cursor, end);
        if (*cursor >= end || **cursor != ':') {
            return false;
        }
        (*cursor)++;
        skip_ws(cursor, end);
        if (strcmp(key, "touch_events") == 0) {
            if (!parse_json_bool(cursor, end, &message->touch_subscribe)) {
                return false;
            }
        } else if (!skip_json_value(cursor, end)) {
            return false;
        }
        skip_ws(cursor, end);
        if (*cursor < end && **cursor == ',') {
            (*cursor)++;
            continue;
        }
        if (*cursor < end && **cursor == '}') {
            (*cursor)++;
            return true;
        }
        return false;
    }
}

bool companion_control_parse(const char *json, size_t length,
                             companion_control_message_t *message)
{
    if (json == NULL || length == 0 || message == NULL) {
        return false;
    }

    const char *cursor = json;
    const char *end = json + length;
    skip_ws(&cursor, end);
    if (cursor >= end || *cursor != '{') {
        return false;
    }
    cursor++;

    companion_control_message_t parsed = {};
    bool has_type = false;
    bool has_request_id = false;

    while (true) {
        char key[32] = {};
        skip_ws(&cursor, end);
        if (cursor < end && *cursor == '}') {
            cursor++;
            break;
        }
        if (!parse_json_string(&cursor, end, key, sizeof(key))) {
            return false;
        }
        skip_ws(&cursor, end);
        if (cursor >= end || *cursor != ':') {
            return false;
        }
        cursor++;
        skip_ws(&cursor, end);

        if (strcmp(key, "type") == 0) {
            if (!parse_json_string(&cursor, end, parsed.type, sizeof(parsed.type))) {
                return false;
            }
            has_type = true;
        } else if (strcmp(key, "request_id") == 0) {
            if (!parse_json_string(&cursor, end, parsed.request_id, sizeof(parsed.request_id))) {
                return false;
            }
            has_request_id = true;
        } else if (strcmp(key, "protocol_version") == 0) {
            if (!parse_json_int(&cursor, end, &parsed.protocol_version)) {
                return false;
            }
        } else if (strcmp(key, "video_transport") == 0) {
            if (!parse_json_string(&cursor, end, parsed.video_transport, sizeof(parsed.video_transport))) {
                return false;
            }
        } else if (strcmp(key, "features") == 0) {
            if (!parse_features_object(&cursor, end, &parsed)) {
                return false;
            }
        } else {
            if (!skip_json_value(&cursor, end)) {
                return false;
            }
        }

        skip_ws(&cursor, end);
        if (cursor < end && *cursor == ',') {
            cursor++;
            continue;
        }
        if (cursor < end && *cursor == '}') {
            cursor++;
            break;
        }
        return false;
    }

    skip_ws(&cursor, end);
    if (cursor != end || !has_type || !has_request_id) {
        return false;
    }
    *message = parsed;
    return true;
}

bool companion_control_transport_supported(const char *transport)
{
    return transport != NULL &&
           (strcmp(transport, COMPANION_CONTROL_TRANSPORT_TCP) == 0 ||
            strcmp(transport, COMPANION_CONTROL_TRANSPORT_UDP) == 0);
}

int companion_control_build_error(char *buffer, size_t buffer_size,
                                  const char *request_id, const char *code)
{
    return snprintf(buffer, buffer_size,
                    "{\"type\":\"error\",\"request_id\":\"%s\",\"code\":\"%s\"}",
                    request_id != NULL ? request_id : "",
                    code != NULL ? code : "");
}

int companion_control_build_hello_ack(char *buffer, size_t buffer_size,
                                      const char *request_id,
                                      const char *video_transport,
                                      size_t max_frame_bytes,
                                      uint16_t udp_port,
                                      uint32_t session_id,
                                      uint32_t session_token,
                                      uint16_t udp_max_payload,
                                      bool touch_enabled)
{
    const char *touch = touch_enabled ? "true" : "false";
    const char *touch_description = touch_enabled
        ? ",\"touch\":{\"width\":%d,\"height\":%d,\"max_move_hz\":%d}"
        : "";
    if (strcmp(video_transport, COMPANION_CONTROL_TRANSPORT_UDP) == 0) {
        int written = snprintf(buffer, buffer_size,
                               "{\"type\":\"hello_ack\",\"request_id\":\"%s\",\"protocol_version\":%d,\"video_transport\":\"udp\",\"width\":%d,\"height\":%d,\"max_frame_bytes\":%u,\"udp\":{\"port\":%u,\"session_id\":%lu,\"session_token\":%lu,\"max_payload\":%u,\"flow_control\":\"latest_frame_ack\",\"max_in_flight_frames\":1,\"feedback_timeout_ms\":250,\"fec\":{\"scheme\":\"xor_interleaved\",\"data_fragments_per_group\":%u}},\"capabilities\":{\"video\":[\"tcp\",\"udp\"],\"frame_feedback\":true,\"touch\":%s,\"audio_capture\":false,\"text_input\":false}",
                               request_id, COMPANION_CONTROL_PROTOCOL_VERSION,
                               COMPANION_CONTROL_WIDTH, COMPANION_CONTROL_HEIGHT,
                               (unsigned)max_frame_bytes, udp_port,
                               (unsigned long)session_id, (unsigned long)session_token,
                               udp_max_payload, COMPANION_UDP_FEC_DATA_FRAGMENTS_PER_GROUP, touch);
        if (touch_enabled && written > 0 && (size_t)written < buffer_size) {
            written += snprintf(buffer + written, buffer_size - (size_t)written,
                                touch_description, COMPANION_CONTROL_WIDTH,
                                COMPANION_CONTROL_HEIGHT, COMPANION_CONTROL_TOUCH_MAX_MOVE_HZ);
        }
        if (written > 0 && (size_t)written < buffer_size) {
            written += snprintf(buffer + written, buffer_size - (size_t)written, "}");
        }
        return written;
    }
    int written = snprintf(buffer, buffer_size,
                           "{\"type\":\"hello_ack\",\"request_id\":\"%s\",\"protocol_version\":%d,\"video_transport\":\"tcp\",\"width\":%d,\"height\":%d,\"max_frame_bytes\":%u,\"capabilities\":{\"video\":[\"tcp\",\"udp\"],\"frame_feedback\":true,\"touch\":%s,\"audio_capture\":false,\"text_input\":false}",
                           request_id, COMPANION_CONTROL_PROTOCOL_VERSION,
                           COMPANION_CONTROL_WIDTH, COMPANION_CONTROL_HEIGHT,
                           (unsigned)max_frame_bytes, touch);
    if (touch_enabled && written > 0 && (size_t)written < buffer_size) {
        written += snprintf(buffer + written, buffer_size - (size_t)written,
                            touch_description, COMPANION_CONTROL_WIDTH,
                            COMPANION_CONTROL_HEIGHT, COMPANION_CONTROL_TOUCH_MAX_MOVE_HZ);
    }
    if (written > 0 && (size_t)written < buffer_size) {
        written += snprintf(buffer + written, buffer_size - (size_t)written, "}");
    }
    return written;
}

int companion_control_build_stream_started(char *buffer, size_t buffer_size,
                                           const char *request_id,
                                           const char *video_transport)
{
    return snprintf(buffer, buffer_size,
                    "{\"type\":\"stream_started\",\"request_id\":\"%s\",\"video_transport\":\"%s\"}",
                    request_id, video_transport);
}

int companion_control_build_stream_stopped(char *buffer, size_t buffer_size,
                                           const char *request_id)
{
    return snprintf(buffer, buffer_size,
                    "{\"type\":\"stream_stopped\",\"request_id\":\"%s\"}",
                    request_id);
}

int companion_control_build_stream_timeout(char *buffer, size_t buffer_size)
{
    return snprintf(buffer, buffer_size,
                    "{\"type\":\"stream_stopped\",\"reason\":\"frame_timeout\"}");
}

int companion_control_build_pong(char *buffer, size_t buffer_size,
                                 const char *request_id)
{
    return snprintf(buffer, buffer_size,
                    "{\"type\":\"pong\",\"request_id\":\"%s\"}",
                    request_id);
}

int companion_control_build_frame_presented(char *buffer, size_t buffer_size,
                                            const frame_pipeline_presented_frame_t *frame)
{
    if (frame == NULL) {
        return -1;
    }
    return snprintf(buffer, buffer_size,
                    "{\"type\":\"frame_presented\",\"sequence\":%lu,\"queue_wait_us\":%lu,\"jpeg_decode_us\":%lu,\"lcd_wait_us\":%lu,\"lcd_submit_us\":%lu,\"previous_dma_us\":%lu,\"replaced_frames\":%lu}",
                    (unsigned long)frame->sequence,
                    (unsigned long)frame->queue_wait_us,
                    (unsigned long)frame->jpeg_decode_us,
                    (unsigned long)frame->lcd_wait_us,
                    (unsigned long)frame->lcd_submit_us,
                    (unsigned long)frame->previous_dma_us,
                    (unsigned long)frame->replaced_frames);
}

int companion_control_build_touch_events(char *buffer, size_t buffer_size,
                                         uint32_t seq,
                                         const touch_input_event_t *events,
                                         size_t count)
{
    if (events == NULL || count == 0 || count > TOUCH_INPUT_MAX_BATCH) {
        return -1;
    }
    static const char *kind_names[] = {"down", "move", "up"};
    int written = snprintf(buffer, buffer_size,
                           "{\"type\":\"touch_events\",\"seq\":%lu,\"events\":[",
                           (unsigned long)seq);
    if (written < 0 || (size_t)written >= buffer_size) {
        return -1;
    }
    for (size_t i = 0; i < count; ++i) {
        if (events[i].kind > TOUCH_INPUT_EVENT_UP) {
            return -1;
        }
        int piece = snprintf(buffer + written, buffer_size - (size_t)written,
                             "%s{\"kind\":\"%s\",\"x\":%u,\"y\":%u,\"t\":%lu}",
                             i > 0 ? "," : "", kind_names[events[i].kind],
                             events[i].x, events[i].y,
                             (unsigned long)events[i].t_ms);
        if (piece < 0 || (size_t)(written + piece) >= buffer_size) {
            return -1;
        }
        written += piece;
    }
    int tail = snprintf(buffer + written, buffer_size - (size_t)written, "]}");
    if (tail < 0 || (size_t)(written + tail) >= buffer_size) {
        return -1;
    }
    return written + tail;
}

int companion_control_build_status(char *buffer, size_t buffer_size,
                                   const companion_status_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return -1;
    }
    return snprintf(buffer, buffer_size,
                    "{\"name\":\"companion-display\",\"width\":%d,\"height\":%d,\"wifi\":\"%s\",\"ipv4\":\"%s\",\"active_transport\":\"%s\",\"token_set\":true,\"protocol_version\":%d,\"video_transports\":[\"tcp\",\"udp\"],\"touch\":%s,\"touch_events_active\":%s,\"audio_capture\":false,\"dropped_frames\":%lu,\"performance\":{\"sequence\":%lu,\"queueWaitUs\":%lu,\"jpegDecodeUs\":%lu,\"lcdWaitUs\":%lu,\"lcdSubmitUs\":%lu,\"previousDmaUs\":%lu,\"replacedFrames\":%lu},\"udp\":{\"rxPackets\":%lu,\"acceptedFragments\":%lu,\"rejectedPackets\":%lu,\"completedFrames\":%lu,\"submittedFrames\":%lu,\"submitFailures\":%lu,\"reassemblyTimeouts\":%lu,\"fecPackets\":%lu,\"fecRecoveredFragments\":%lu,\"fecUnrecoverableFrames\":%lu,\"lastReject\":\"%s\",\"lastOutcome\":\"%s\",\"lastOutcomeSequence\":%lu,\"lastReassemblyUs\":%lu}}",
                    COMPANION_CONTROL_WIDTH, COMPANION_CONTROL_HEIGHT,
                    snapshot->wifi != NULL ? snapshot->wifi : "",
                    snapshot->ipv4 != NULL ? snapshot->ipv4 : "",
                    snapshot->active_transport != NULL ? snapshot->active_transport : "none",
                    COMPANION_CONTROL_PROTOCOL_VERSION,
                    snapshot->touch_hw ? "true" : "false",
                    snapshot->touch_events_active ? "true" : "false",
                    (unsigned long)snapshot->dropped_frames,
                    (unsigned long)snapshot->performance.sequence,
                    (unsigned long)snapshot->performance.queue_wait_us,
                    (unsigned long)snapshot->performance.jpeg_decode_us,
                    (unsigned long)snapshot->performance.lcd_wait_us,
                    (unsigned long)snapshot->performance.lcd_submit_us,
                    (unsigned long)snapshot->performance.previous_dma_us,
                    (unsigned long)snapshot->performance.replaced_frames,
                    (unsigned long)snapshot->udp_rx_packets,
                    (unsigned long)snapshot->udp_accepted_fragments,
                    (unsigned long)snapshot->udp_rejected_packets,
                    (unsigned long)snapshot->udp_completed_frames,
                    (unsigned long)snapshot->udp_submitted_frames,
                    (unsigned long)snapshot->udp_submit_failures,
                    (unsigned long)snapshot->udp_reassembly_timeouts,
                    (unsigned long)snapshot->udp_fec_packets,
                    (unsigned long)snapshot->udp_fec_recovered_fragments,
                    (unsigned long)snapshot->udp_fec_unrecoverable_frames,
                    snapshot->udp_last_reject != NULL ? snapshot->udp_last_reject : "none",
                    snapshot->udp_last_outcome != NULL ? snapshot->udp_last_outcome : "none",
                    (unsigned long)snapshot->udp_last_outcome_sequence,
                    (unsigned long)snapshot->udp_last_reassembly_us);
}

int companion_control_build_udp_feedback(char *buffer, size_t buffer_size,
                                         uint32_t latest_sequence,
                                         uint32_t completed_frames,
                                         uint32_t submitted_frames,
                                         uint32_t recovered_fragments,
                                         uint32_t unrecoverable_frames,
                                         const char *outcome,
                                         uint32_t outcome_sequence,
                                         uint32_t reassembly_us)
{
    int written = snprintf(buffer, buffer_size,
                           "{\"type\":\"udp_feedback\",\"latest_sequence\":%lu,\"completed_frames\":%lu,\"submitted_frames\":%lu,\"recovered_fragments\":%lu,\"unrecoverable_frames\":%lu",
                           (unsigned long)latest_sequence,
                           (unsigned long)completed_frames,
                           (unsigned long)submitted_frames,
                           (unsigned long)recovered_fragments,
                           (unsigned long)unrecoverable_frames);
    if (written < 0 || (size_t)written >= buffer_size) return -1;
    if (outcome != NULL) {
        int fields = snprintf(buffer + written, buffer_size - (size_t)written,
                              ",\"outcome\":\"%s\",\"outcome_sequence\":%lu,\"reassembly_us\":%lu",
                              outcome, (unsigned long)outcome_sequence,
                              (unsigned long)reassembly_us);
        if (fields < 0 || (size_t)fields >= buffer_size - (size_t)written) return -1;
        written += fields;
    }
    int tail = snprintf(buffer + written, buffer_size - (size_t)written, "}");
    if (tail < 0 || (size_t)tail >= buffer_size - (size_t)written) return -1;
    return written + tail;
}
