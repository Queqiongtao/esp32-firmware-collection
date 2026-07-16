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
#include "test_common.h"

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

typedef enum {
    TEST_UVC_STATE_IDLE = 0,
    TEST_UVC_STATE_HOST_READY,
    TEST_UVC_STATE_ENUMERATING,
    TEST_UVC_STATE_STREAMING,
    TEST_UVC_STATE_RECOVERING,
    TEST_UVC_STATE_FAILED,
} test_uvc_state_t;

typedef enum {
    TEST_UVC_RECOVERY_NONE = 0,
    TEST_UVC_RECOVERY_STREAM,
    TEST_UVC_RECOVERY_ROOT_PORT,
    TEST_UVC_RECOVERY_STACK,
} test_uvc_recovery_level_t;

typedef enum {
    TEST_UVC_PREVIEW_BACKEND_CPU = 0,
    TEST_UVC_PREVIEW_BACKEND_PPA,
} test_uvc_preview_backend_t;

typedef struct {
    test_module_meta_t module;
    test_uvc_state_t state;
    bool initialized;
    bool controller_released;
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
    uint32_t frames_dropped;
    uint32_t frame_queue_high_water;
    uint32_t last_frame_bytes;
    uint32_t last_decode_us;
    uint32_t last_preview_us;
    uint32_t last_frame_w;
    uint32_t last_frame_h;
    int64_t last_frame_time_us;
    test_uvc_recovery_level_t recovery_level;
    uint32_t recovery_attempts;
    bool recovery_required;
    bool warm_recovery_expected;
    test_uvc_preview_backend_t preview_backend;
    uint32_t preview_fallbacks;
    uint32_t transfer_errors;
    uint32_t frame_overflows;
    uint32_t frame_underflows;
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

/** @brief Recover a stalled UVC stream at the requested recovery level. */
esp_err_t test_uvc_recover(test_uvc_recovery_level_t level);

/** @brief Return whether a warm reset interrupted an active UVC stream. */
bool test_uvc_should_resume_after_reset(void);

/** @brief Clear the warm-reset resume marker after an intentional role exit. */
void test_uvc_clear_resume_marker(void);

/**
 * @brief Copy the newest scaled RGB565 preview into a caller-owned buffer.
 *
 * The buffer must be TEST_UVC_PREVIEW_H_RES * TEST_UVC_PREVIEW_V_RES * 2
 * bytes. If out_info->frame_id is non-zero, the copy is skipped when that is
 * already the newest frame. Returns ESP_ERR_NOT_FOUND when no newer frame is ready.
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
