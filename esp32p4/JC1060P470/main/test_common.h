/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    TEST_MODULE_STATE_UNINITIALIZED = 0,
    TEST_MODULE_STATE_INITIALIZING,
    TEST_MODULE_STATE_READY,
    TEST_MODULE_STATE_RUNNING,
    TEST_MODULE_STATE_STOPPING,
    TEST_MODULE_STATE_ERROR,
} test_module_state_t;

typedef struct {
    test_module_state_t state;
    esp_err_t last_error;
    /** Incremented only after a new module lifecycle initializes successfully. */
    uint32_t generation;
    /** Timestamp of the latest externally observable state change. */
    int64_t updated_at_us;
} test_module_meta_t;
