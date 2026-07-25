#include "companion_udp.h"

#include "frame_pipeline.h"

companion_udp_reject_reason_t companion_udp_validate_packet(
    const companion_udp_header_t *header,
    size_t packet_payload,
    size_t max_frame_bytes,
    bool session_active,
    uint32_t session_id,
    uint32_t session_token,
    bool peer_addr_bound,
    uint32_t peer_addr,
    uint32_t source_addr)
{
    if (!session_active) {
        return COMPANION_UDP_REJECT_INACTIVE;
    }
    if (header == NULL || header->magic != COMPANION_UDP_MAGIC ||
        header->version != COMPANION_FRAME_VERSION ||
        header->stream_type != COMPANION_UDP_STREAM_VIDEO ||
        header->header_size != sizeof(*header)) {
        return COMPANION_UDP_REJECT_HEADER;
    }
    if (header->session_id != session_id || header->session_token != session_token) {
        return COMPANION_UDP_REJECT_SESSION;
    }
    if (header->fragment_count == 0 ||
        header->fragment_count > COMPANION_UDP_MAX_FRAGMENTS ||
        header->fragment_index >= header->fragment_count) {
        return COMPANION_UDP_REJECT_FRAGMENT;
    }
    if (header->payload_length != packet_payload ||
        header->payload_length > COMPANION_UDP_MAX_PAYLOAD ||
        header->frame_length < sizeof(companion_frame_header_t) ||
        header->frame_length > max_frame_bytes ||
        header->fragment_offset > header->frame_length ||
        header->payload_length > header->frame_length - header->fragment_offset) {
        return COMPANION_UDP_REJECT_LENGTH;
    }
    if (peer_addr_bound && source_addr != peer_addr) {
        return COMPANION_UDP_REJECT_SOURCE;
    }
    return COMPANION_UDP_REJECT_NONE;
}

const char *companion_udp_reject_reason_name(companion_udp_reject_reason_t reason)
{
    switch (reason) {
    case COMPANION_UDP_REJECT_INACTIVE: return "inactive";
    case COMPANION_UDP_REJECT_SOURCE: return "source_ip";
    case COMPANION_UDP_REJECT_HEADER: return "header";
    case COMPANION_UDP_REJECT_SESSION: return "session";
    case COMPANION_UDP_REJECT_FRAGMENT: return "fragment";
    case COMPANION_UDP_REJECT_LENGTH: return "length";
    case COMPANION_UDP_REJECT_NONE: default: return "none";
    }
}
