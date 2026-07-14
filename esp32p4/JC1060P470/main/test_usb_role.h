/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    TEST_USB_ROLE_NONE = 0,
    TEST_USB_ROLE_CDC_DEVICE,   /**< TinyUSB HS Device CDC-ACM echo (test_usb) */
    TEST_USB_ROLE_UVC_HOST,     /**< USB Host UVC camera + hardware JPEG decode (test_uvc) */
} test_usb_role_t;

typedef struct {
    test_usb_role_t current_role;       /**< Currently active role (NONE if idle) */
    test_usb_role_t target_role;        /**< Role being switched to (equals current when idle) */
    bool switch_in_progress;           /**< True while a role switch is in progress */
    bool cdc_attached;                 /**< CDC Device: host enumerated */
    bool uvc_device_connected;         /**< UVC Host: camera connected */
    bool uvc_stream_started;           /**< UVC Host: stream open and started */
    bool uvc_format_negotiated;        /**< UVC Host: format negotiated from device descriptors */
    uint32_t uvc_negotiated_h_res;     /**< UVC Host: negotiated horizontal resolution */
    uint32_t uvc_negotiated_v_res;     /**< UVC Host: negotiated vertical resolution */
    float uvc_negotiated_fps;          /**< UVC Host: negotiated frames per second */
    uint32_t uvc_frames_decoded;       /**< UVC Host: total frames decoded */
    uint32_t uvc_decode_errors;        /**< UVC Host: total decode errors */
    uint32_t cdc_rx_packets;           /**< CDC Device: total RX packets */
    uint32_t cdc_tx_packets;           /**< CDC Device: total TX packets */
    esp_err_t last_switch_error;       /**< Error from the most recent switch attempt */
} test_usb_role_status_t;

/**
 * @brief Initialize the USB role management framework. Creates the role worker
 *        task in the idle state. UI actions select CDC Device or UVC Host
 *        after startup so controller[0] is not occupied unnecessarily.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_usb_role_init(void);

/**
 * @brief Asynchronously request a switch to the target role. The actual
 *        switch (deinit current -> init target) runs in the role worker task
 *        to avoid blocking the caller (e.g. the LVGL UI task). If the target
 *        equals the current role, the request is a no-op.
 *
 * @param target  The role to switch to.
 * @return ESP_OK if the switch request was queued, ESP_ERR_INVALID_ARG for
 *         invalid role, ESP_ERR_NO_MEM if the queue is full.
 */
esp_err_t test_usb_role_switch(test_usb_role_t target);

/**
 * @brief Select a UVC resolution and restart the UVC Host role asynchronously.
 *
 * The request is serialized with all other USB role changes.
 */
esp_err_t test_usb_role_restart_uvc(uint32_t h_res, uint32_t v_res);

/**
 * @brief Get the currently active role.
 *
 * @return The current role, or TEST_USB_ROLE_NONE if not initialized.
 */
test_usb_role_t test_usb_role_current(void);

/**
 * @brief Get a snapshot of the USB role management state, mirroring the
 *        underlying test_usb / test_uvc module status.
 *
 * @param out_status  Output status buffer.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out_status is NULL,
 *         ESP_ERR_INVALID_STATE if the module has not been initialized.
 */
esp_err_t test_usb_role_get_status(test_usb_role_status_t *out_status);
