#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct subscreen_display subscreen_display_t;

typedef enum {
    SUBSCREEN_TRANSPORT_READY,
    SUBSCREEN_TRANSPORT_WAITING_FRAME,
    SUBSCREEN_TRANSPORT_RECEIVING_FRAME,
    SUBSCREEN_TRANSPORT_FRAME_REJECTED,
    SUBSCREEN_TRANSPORT_FRAME_FAILED,
} subscreen_transport_status_t;

typedef enum {
    SUBSCREEN_NETWORK_SETUP_REQUIRED,
    SUBSCREEN_NETWORK_CONNECTING,
    SUBSCREEN_NETWORK_CONNECTED,
} subscreen_network_status_t;

typedef struct {
    uint32_t jpeg_decode_us;
    uint32_t lcd_wait_us;
    uint32_t lcd_submit_us;
    uint32_t previous_dma_us;
} subscreen_display_frame_metrics_t;

esp_err_t subscreen_display_init(subscreen_display_t **ret_display);
esp_err_t subscreen_display_show_placeholder(subscreen_display_t *display);
esp_err_t subscreen_display_set_transport_status(subscreen_display_t *display,
                                                 subscreen_transport_status_t status);
esp_err_t subscreen_display_set_network_status(subscreen_display_t *display,
                                               subscreen_network_status_t status,
                                               const char *ipv4_address);
bool subscreen_display_supports_frame_size(const subscreen_display_t *display,
                                           uint16_t width,
                                           uint16_t height);
esp_err_t subscreen_display_draw_jpeg(subscreen_display_t *display,
                                      const uint8_t *jpeg,
                                      size_t jpeg_len,
                                      uint16_t width,
                                      uint16_t height,
                                      subscreen_display_frame_metrics_t *metrics);
bool subscreen_display_has_frame(const subscreen_display_t *display);

#ifdef __cplusplus
}
#endif
