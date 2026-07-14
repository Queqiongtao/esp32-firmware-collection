/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t h_res;
    uint32_t v_res;
    float fps;
} test_uvc_resolution_t;

typedef struct {
    bool valid;
    uint32_t frame_id;
    uint32_t h_res;
    uint32_t v_res;
} test_uvc_preview_info_t;

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
 * @brief Get MJPEG resolutions reported by the current UVC device.
 *
 * Pass a NULL list to query the number of available entries. The input count
 * is the destination capacity; on return it is the total available count.
 */
esp_err_t test_uvc_get_resolutions(test_uvc_resolution_t *out_list, size_t *inout_count);

/** @brief Select the resolution to use on the next UVC Host restart. */
esp_err_t test_uvc_set_preferred_resolution(uint32_t h_res, uint32_t v_res);

/** @brief Change the active UVC stream resolution without reinstalling USB Host. */
esp_err_t test_uvc_change_resolution(uint32_t h_res, uint32_t v_res);

/**
 * @brief Copy the newest scaled RGB565 preview into a caller-owned buffer.
 *
 * The buffer must be TEST_UVC_PREVIEW_H_RES * TEST_UVC_PREVIEW_V_RES * 2
 * bytes. Returns ESP_ERR_NOT_FOUND until the first decoded frame is ready.
 */
esp_err_t test_uvc_copy_preview(void *out_rgb565, size_t out_size,
                                test_uvc_preview_info_t *out_info);

/**
 * @brief Stop and close the UVC stream (if open), uninstall the UVC driver,
 *        tear down the USB Host Library, release the JPEG decoder, and delete
 *        the worker task. Safe to call when the module has not been
 *        initialized.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_uvc_deinit(void);
