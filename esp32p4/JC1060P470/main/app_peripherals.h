/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t enabled_count;
    uint32_t ready_count;
    uint32_t failed_count;
    esp_err_t first_error;
} app_peripherals_result_t;

/**
 * Probe and start all test capabilities enabled by Kconfig once during boot.
 * Calls made after startup completes return the cached result without touching
 * peripheral resources again.
 */
esp_err_t app_peripherals_start(esp_err_t nvs_status, app_peripherals_result_t *out_result);
