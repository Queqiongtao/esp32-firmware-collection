#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COMPANION_UDP_MAGIC 0x31554443UL /* CDU1, little-endian */
#define COMPANION_UDP_STREAM_VIDEO 1
#define COMPANION_UDP_MAX_PAYLOAD 1200U
#define COMPANION_UDP_MAX_FRAGMENTS 128U
#define COMPANION_UDP_FEC_FLAG 0x8000U
#define COMPANION_UDP_FEC_DATA_FRAGMENTS_PER_GROUP 8U
#define COMPANION_UDP_MAX_FEC_GROUPS \
    (COMPANION_UDP_MAX_FRAGMENTS / COMPANION_UDP_FEC_DATA_FRAGMENTS_PER_GROUP)
#define COMPANION_UDP_FRAGMENTED_FRAME_LIMIT \
    ((size_t)COMPANION_UDP_MAX_PAYLOAD * COMPANION_UDP_MAX_FRAGMENTS)

typedef enum {
    COMPANION_UDP_REJECT_NONE,
    COMPANION_UDP_REJECT_INACTIVE,
    COMPANION_UDP_REJECT_SOURCE,
    COMPANION_UDP_REJECT_HEADER,
    COMPANION_UDP_REJECT_SESSION,
    COMPANION_UDP_REJECT_FRAGMENT,
    COMPANION_UDP_REJECT_LENGTH,
} companion_udp_reject_reason_t;

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

static inline uint16_t companion_udp_fec_group_count(uint16_t fragment_count)
{
    return (fragment_count + COMPANION_UDP_FEC_DATA_FRAGMENTS_PER_GROUP - 1U) /
           COMPANION_UDP_FEC_DATA_FRAGMENTS_PER_GROUP;
}

static inline bool companion_udp_is_fec(const companion_udp_header_t *header)
{
    return header != NULL && (header->reserved & COMPANION_UDP_FEC_FLAG) != 0;
}

companion_udp_reject_reason_t companion_udp_validate_packet(
    const companion_udp_header_t *header,
    size_t packet_payload,
    size_t max_frame_bytes,
    bool session_active,
    uint32_t session_id,
    uint32_t session_token,
    bool peer_addr_bound,
    uint32_t peer_addr,
    uint32_t source_addr);

const char *companion_udp_reject_reason_name(companion_udp_reject_reason_t reason);

#ifdef __cplusplus
}
#endif
