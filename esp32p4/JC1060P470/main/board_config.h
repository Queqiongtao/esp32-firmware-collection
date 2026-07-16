/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#define BOARD_SHARED_I2C_PORT                    I2C_NUM_1
#define BOARD_SHARED_I2C_SDA                     GPIO_NUM_7
#define BOARD_SHARED_I2C_SCL                     GPIO_NUM_8
#define BOARD_SHARED_I2C_TOUCH_RTC_CLK_HZ        400000
