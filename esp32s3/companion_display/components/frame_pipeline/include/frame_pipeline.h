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
    COMPANION_TRANSPORT_TCP_VIDEO,
    COMPANION_TRANSPORT_UDP_VIDEO,
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

/* CDP1 uses the same CRC-32 value as crc32fast::hash(); esp_crc32_le() starts with 0. */

typedef enum {
    FRAME_PIPELINE_SUBMIT_OK,
    FRAME_PIPELINE_SUBMIT_INVALID_ARG,
    FRAME_PIPELINE_SUBMIT_BUSY,
    FRAME_PIPELINE_SUBMIT_BAD_HEADER,
    FRAME_PIPELINE_SUBMIT_BAD_CRC,
    FRAME_PIPELINE_SUBMIT_NO_BUFFER,
} frame_pipeline_submit_result_t;

typedef struct {
    uint32_t sequence;
    uint32_t queue_wait_us;
    uint32_t jpeg_decode_us;
    uint32_t lcd_wait_us;
    uint32_t lcd_submit_us;
    uint32_t previous_dma_us;
    uint32_t replaced_frames;
} frame_pipeline_presented_frame_t;

typedef void (*frame_pipeline_presented_callback_t)(const frame_pipeline_presented_frame_t *frame,
                                                    void *context);

esp_err_t frame_pipeline_init(struct subscreen_display *display);
frame_pipeline_submit_result_t frame_pipeline_submit(companion_transport_t source,
                                                     const companion_frame_header_t *header,
                                                     const uint8_t *payload);
esp_err_t frame_pipeline_submit_result_to_esp_err(frame_pipeline_submit_result_t result);
const char *frame_pipeline_submit_result_name(frame_pipeline_submit_result_t result);
bool frame_pipeline_acquire(companion_transport_t source);
/**
 * @brief Stop accepting frames from a transport and synchronously invalidate
 *        queued/in-flight work before returning.
 *
 * A frame already being drawn is allowed to finish. Queued frames from the
 * released generation are discarded and cannot overwrite a subsequent
 * placeholder screen.
 */
esp_err_t frame_pipeline_release(companion_transport_t source);
companion_transport_t frame_pipeline_active_transport(void);
void frame_pipeline_set_presented_callback(frame_pipeline_presented_callback_t callback, void *context);

#ifdef __cplusplus
}
#endif
