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
    bool initialized;
    bool attached;
    bool dtr;
    bool rts;
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    size_t last_rx_size;
    size_t last_tx_size;
    esp_err_t last_error;
} test_usb_status_t;

/**
 * @brief Install the TinyUSB device stack and initialize one CDC-ACM port on
 *        the board's High-Speed USB Device interface. RX events are handled
 *        asynchronously by the module's worker task, which logs a hex dump and
 *        echoes received data back to the host.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_usb_init(void);

/**
 * @brief Compatibility hook for the existing app_main flow. The CDC demo starts
 *        running from test_usb_init(), so this function only validates that the
 *        module is initialized and returns immediately.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_usb_run(void);

/**
 * @brief Get a snapshot of the USB CDC-ACM module state.
 *
 * @param out_status  Output status buffer.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out_status is NULL.
 */
esp_err_t test_usb_get_status(test_usb_status_t *out_status);

/**
 * @brief Deinitialize the CDC-ACM interface, uninstall TinyUSB, and release
 *        the application queue/task. Safe to call when USB has not been
 *        initialized.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_usb_deinit(void);
