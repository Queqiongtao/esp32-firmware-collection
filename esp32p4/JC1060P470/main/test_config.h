/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "driver/gpio.h"

/* ---------------------------- SD card (SDMMC) ------------------------------ *
 * Board wiring (JC1060P470, same as ESP32-P4 Function EV Board):
 *   D0-D3 = GPIO39-42, CLK = GPIO43, CMD = GPIO44
 *
 * These are SLOT_0 IO MUX dedicated UHS-I pins, connected directly to the
 * SDMMC controller (not through the GPIO matrix). SDMMC IO power is supplied
 * by the on-chip LDO channel 4 (LDO_VO4). Channel 3 is occupied by the MIPI
 * DSI PHY (2.5V). */

#define TEST_SDCARD_PIN_CLK                     GPIO_NUM_43
#define TEST_SDCARD_PIN_CMD                     GPIO_NUM_44
#define TEST_SDCARD_PIN_D0                      GPIO_NUM_39
#define TEST_SDCARD_PIN_D1                      GPIO_NUM_40
#define TEST_SDCARD_PIN_D2                      GPIO_NUM_41
#define TEST_SDCARD_PIN_D3                      GPIO_NUM_42

#define TEST_SDCARD_BUS_WIDTH                   4

#define TEST_SDCARD_PWR_LDO_CHAN                4

#define TEST_SDCARD_MOUNT_POINT                 "/sdcard"
#define TEST_SDCARD_MAX_FILES                   5
#define TEST_SDCARD_ALLOCATION_UNIT_SIZE        (64 * 1024)
