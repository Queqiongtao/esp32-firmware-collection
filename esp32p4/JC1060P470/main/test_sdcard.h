/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"

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
 * @brief Unmount the FAT filesystem and release the SDMMC power control
 *        driver. Safe to call when the card has not been initialized.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_sdcard_deinit(void);
