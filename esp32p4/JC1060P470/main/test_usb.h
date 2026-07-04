/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"

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
 * @brief Deinitialize the CDC-ACM interface, uninstall TinyUSB, and release
 *        the application queue/task. Safe to call when USB has not been
 *        initialized.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_usb_deinit(void);
