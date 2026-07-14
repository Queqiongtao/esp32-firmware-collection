/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool initialized;
    bool stream_opened;
    bool stream_started;
    bool device_connected;
    bool format_negotiated;
    uint32_t negotiated_h_res;
    uint32_t negotiated_v_res;
    float negotiated_fps;
    uint32_t frames_received;
    uint32_t frames_decoded;
    uint32_t decode_errors;
    uint32_t last_frame_bytes;
    uint32_t last_decode_us;
    uint32_t last_frame_w;
    uint32_t last_frame_h;
    esp_err_t last_error;
} test_uvc_status_t;

/**
 * @brief Install the USB Host Library on the board's High-Speed USB OTG
 *        peripheral, install the esp-usb UVC host driver, acquire the ESP32-P4
 *        hardware JPEG decoder, and start the background USB Host event task
 *        and the frame worker task. The worker task remains idle until
 *        test_uvc_run() opens a UVC stream.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_uvc_init(void);

/**
 * @brief Open and start a UVC stream from any attached UVC camera. This is a
 *        blocking call that waits for a UVC device, performs bounded root-port
 *        recovery and negotiates the stream format. On success the frame callback starts
 *        enqueueing frames for the worker task, which decodes them with the
 *        hardware JPEG decoder and logs periodic stream summaries.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_uvc_run(void);

/**
 * @brief Get a snapshot of the UVC camera test module state.
 *
 * @param out_status  Output status buffer.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out_status is NULL.
 */
esp_err_t test_uvc_get_status(test_uvc_status_t *out_status);

/**
 * @brief Stop and close the UVC stream (if open), uninstall the UVC driver,
 *        tear down the USB Host Library, release the JPEG decoder, and delete
 *        the worker task. Safe to call when the module has not been
 *        initialized.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_uvc_deinit(void);
