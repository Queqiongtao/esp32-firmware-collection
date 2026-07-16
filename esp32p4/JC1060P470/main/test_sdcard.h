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
    test_module_meta_t module;
    bool initialized;
    bool mounted;
    bool test_passed;
    uint32_t run_count;
    size_t last_bytes_written;
    size_t last_bytes_read;
    uint32_t last_write_checksum;
    uint32_t last_read_checksum;
    esp_err_t last_error;
} test_sdcard_status_t;

/**
 * @brief Initialize the SDMMC host, on-chip LDO power, and mount the FAT
 *        filesystem. Card info is printed to the log on success.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_sdcard_init(void);

/**
 * @brief Run a simple write/read/verify test against a file on the SD card.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_sdcard_run(void);

/**
 * @brief Copy a consistent snapshot of the SD card module state.
 *
 * @param out_status  Output status buffer.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out_status is NULL.
 */
esp_err_t test_sdcard_get_status(test_sdcard_status_t *out_status);

/**
 * @brief Unmount the FAT filesystem and release the SDMMC power control
 *        driver. Safe to call when the card has not been initialized.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_sdcard_deinit(void);
