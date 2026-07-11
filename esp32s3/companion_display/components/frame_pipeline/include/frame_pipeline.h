#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

struct subscreen_display;

typedef enum {
    COMPANION_TRANSPORT_NONE,
    COMPANION_TRANSPORT_USB,
    COMPANION_TRANSPORT_WIFI,
} companion_transport_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t format;
    uint16_t header_size;
    uint32_t sequence;
    uint16_t width;
    uint16_t height;
    uint32_t payload_size;
    uint32_t crc32;
} companion_frame_header_t;

#define COMPANION_FRAME_MAGIC 0x31504443UL /* CDP1, little-endian */
#define COMPANION_FRAME_VERSION 1
#define COMPANION_FRAME_FORMAT_JPEG 1

esp_err_t frame_pipeline_init(struct subscreen_display *display);
esp_err_t frame_pipeline_submit(companion_transport_t source,
                                const companion_frame_header_t *header,
                                const uint8_t *payload);
bool frame_pipeline_acquire(companion_transport_t source);
void frame_pipeline_release(companion_transport_t source);
companion_transport_t frame_pipeline_active_transport(void);

#ifdef __cplusplus
}
#endif
