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

/* ---------------------------- Audio (I2S + ES8311) ------------------------- *
 * Board wiring (JC1060P470, same as ESP32-P4 Function EV Board, FIB type):
 *   I2S MCLK = GPIO13, BCLK = GPIO12, WS = GPIO10
 *   I2S DOUT = GPIO9 (-> ES8311), DIN = GPIO11 (<- ES8311)
 *   PA (power amplifier enable) = GPIO20
 *
 * ES8311 codec shares the touch I2C bus (I2C_NUM_1, GPIO7/8) via the
 * display_touch module's bus handle getter. */

#define TEST_AUDIO_I2S_NUM                      0
#define TEST_AUDIO_PIN_MCLK                     GPIO_NUM_13
#define TEST_AUDIO_PIN_BCLK                     GPIO_NUM_12
#define TEST_AUDIO_PIN_WS                       GPIO_NUM_10
#define TEST_AUDIO_PIN_DOUT                     GPIO_NUM_9
#define TEST_AUDIO_PIN_DIN                      GPIO_NUM_11
#define TEST_AUDIO_PIN_PA                       GPIO_NUM_20

#define TEST_AUDIO_SAMPLE_RATE                  16000
#define TEST_AUDIO_MCLK_MULTIPLE                384
#define TEST_AUDIO_RECV_BUF_SIZE                (2400)
#ifdef CONFIG_EXAMPLE_VOICE_VOLUME
#define TEST_AUDIO_VOICE_VOLUME                 CONFIG_EXAMPLE_VOICE_VOLUME
#else
#define TEST_AUDIO_VOICE_VOLUME                 60
#endif
#if CONFIG_EXAMPLE_AUDIO_MODE_ECHO
#ifdef CONFIG_EXAMPLE_MIC_GAIN
#define TEST_AUDIO_MIC_GAIN                     CONFIG_EXAMPLE_MIC_GAIN
#else
#define TEST_AUDIO_MIC_GAIN                     3
#endif
#endif
