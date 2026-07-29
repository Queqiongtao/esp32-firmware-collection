/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

/** Create the process-lifetime board I2C bus before initializing any device. */
esp_err_t board_i2c_init(void);
esp_err_t board_i2c_deinit(void);

/** Borrow the board-owned bus; callers must not delete the returned handle. */
esp_err_t board_i2c_get_bus(i2c_master_bus_handle_t *out_handle, i2c_port_num_t *out_port);
