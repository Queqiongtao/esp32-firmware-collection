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
        header->fragment_count > COMPANION_UDP_MAX_FRAGMENTS) {
        return COMPANION_UDP_REJECT_FRAGMENT;
    }
    const bool fec = companion_udp_is_fec(header);
    const uint16_t fec_groups = companion_udp_fec_group_count(header->fragment_count);
    const uint16_t expected_fragments =
        (header->frame_length + COMPANION_UDP_MAX_PAYLOAD - 1U) / COMPANION_UDP_MAX_PAYLOAD;
    const size_t expected_offset = (size_t)header->fragment_index * COMPANION_UDP_MAX_PAYLOAD;
    const size_t expected_payload = expected_offset < header->frame_length
        ? ((header->frame_length - expected_offset < COMPANION_UDP_MAX_PAYLOAD)
               ? header->frame_length - expected_offset
               : COMPANION_UDP_MAX_PAYLOAD)
        : 0;
    if (header->fragment_count != expected_fragments ||
        (!fec && (header->reserved != 0 || header->fragment_index >= header->fragment_count)) ||
        (fec && (header->reserved != (uint16_t)(COMPANION_UDP_FEC_FLAG | header->fragment_index) ||
                 header->fragment_index >= fec_groups || header->fragment_offset != 0))) {
        return COMPANION_UDP_REJECT_FRAGMENT;
    }
    if (header->payload_length != packet_payload ||
        header->payload_length > COMPANION_UDP_MAX_PAYLOAD ||
        header->frame_length < sizeof(companion_frame_header_t) ||
        header->frame_length > max_frame_bytes ||
        (fec && header->payload_length != COMPANION_UDP_MAX_PAYLOAD) ||
        (!fec && (header->fragment_offset > header->frame_length ||
                  header->payload_length > header->frame_length - header->fragment_offset ||
                  header->fragment_offset != expected_offset ||
                  header->payload_length != expected_payload))) {
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
