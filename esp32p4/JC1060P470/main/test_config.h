/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"

#define TEST_AUDIO_BYTES_PER_SAMPLE             2
#define TEST_AUDIO_CHANNELS                     2

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

/* ---------------------------- RTC (RX8025T-UC) ---------------------------- *
 * Board wiring:
 *   RX8025T-UC RTC shares the touch I2C bus (I2C_NUM_1, GPIO7/8) via the
 *   display_touch module's bus handle getter. */

#define TEST_RTC_CHIP_NAME                      "RX8025T-UC"
#define TEST_RTC_I2C_ADDR                       0x32
#define TEST_RTC_I2C_TIMEOUT_MS                 100
#define TEST_RTC_READ_INTERVAL_MS               5000

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
#define TEST_AUDIO_RECORD_SECONDS               5
#define TEST_AUDIO_RECORD_BUF_SIZE              (TEST_AUDIO_SAMPLE_RATE * TEST_AUDIO_BYTES_PER_SAMPLE * \
                                                 TEST_AUDIO_CHANNELS * TEST_AUDIO_RECORD_SECONDS)
#define TEST_AUDIO_TASK_STACK_SIZE              4096
#define TEST_AUDIO_TASK_PRIORITY                4
#define TEST_AUDIO_TASK_CORE_ID                 tskNO_AFFINITY
#ifdef CONFIG_EXAMPLE_VOICE_VOLUME
#define TEST_AUDIO_VOICE_VOLUME                 CONFIG_EXAMPLE_VOICE_VOLUME
#else
#define TEST_AUDIO_VOICE_VOLUME                 60
#endif
#ifdef CONFIG_EXAMPLE_MIC_GAIN
#define TEST_AUDIO_MIC_GAIN                     CONFIG_EXAMPLE_MIC_GAIN
#else
#define TEST_AUDIO_MIC_GAIN                     3
#endif

/* ---------------------------- Battery monitor ------------------------------ *
 * Board wiring:
 *   BAT -> 68K -> GPIO52(sample point) -> 100K -> GND
 *
 * The ADC sees the divider midpoint. Battery voltage is recovered in software
 * with: Vbat = Vadc * (68K + 100K) / 100K = Vadc * 1.68
 *
 * The first-stage percent estimate is only for demo observation. It linearly
 * maps 3.3V to 0% and 4.2V to 100%; it is not a precision fuel gauge. */

#define TEST_BATTERY_ADC_UNIT                    ADC_UNIT_2
#define TEST_BATTERY_ADC_CHANNEL                 ADC_CHANNEL_3
#define TEST_BATTERY_ADC_GPIO                    52
#define TEST_BATTERY_ADC_ATTEN                   ADC_ATTEN_DB_12
#define TEST_BATTERY_ADC_BITWIDTH                ADC_BITWIDTH_DEFAULT
#define TEST_BATTERY_ADC_RAW_FULL_SCALE          4095
#define TEST_BATTERY_ADC_FALLBACK_REF_MV         3300

#define TEST_BATTERY_DIVIDER_R_TOP_OHM           68000
#define TEST_BATTERY_DIVIDER_R_BOTTOM_OHM        100000

#define TEST_BATTERY_SAMPLE_COUNT                16
#define TEST_BATTERY_UPDATE_PERIOD_MS            1000
#define TEST_BATTERY_EMPTY_MV                    3300
#define TEST_BATTERY_FULL_MV                     4200

/* ---------------------------- USB Device CDC-ACM --------------------------- *
 * Board wiring (JC1060P470, FIB type, two USB Type-C ports):
 *   The current board exposes two dedicated USB Device ports:
 *   - Full-Speed USB Device: routed to dedicated USB package pins 49/50 (DM/DP)
 *   - High-Speed USB Device: GPIO24/25, routed to package pins 52/53
 *   The CDC-ACM demo uses the High-Speed USB Device port. esp_tinyusb's
 *   TINYUSB_DEFAULT_CONFIG() selects the ESP32-P4 high-speed device controller
 *   automatically, and tinyusb_driver_install() configures the PHY and spawns
 *   the TinyUSB backend task. No application GPIO assignment is required. 
 *
 * NOTE: The USB Device CDC demo port and the USB Serial/JTAG download/log port
 *   may both enumerate as serial devices on the host side, but they serve
 *   different purposes. Do not confuse the CDC demo port with the download/log
 *   interface.
 *
 * Demo behavior:
 *   The board enumerates as a single CDC-ACM serial device. Data received
 *   from the host is dumped in hex and then echoed back unchanged. */

#define TEST_USB_CDC_PORT                       TINYUSB_CDC_ACM_0
#define TEST_USB_CDC_EVENT_QUEUE_LEN            16
#define TEST_USB_CDC_RX_BUF_SIZE                64
#define TEST_USB_CDC_TX_FLUSH_TIMEOUT_TICKS     0
#define TEST_USB_CDC_TASK_STACK_SIZE            4096
#define TEST_USB_CDC_TASK_PRIORITY              4
#define TEST_USB_CDC_TASK_CORE_ID               tskNO_AFFINITY

/* ---------------------------- Wireless (ESP32-C6 hosted) ------------------ *
 * Board wiring:
 *   ESP32-P4 <-> ESP32-C6 SDIO
 *   D0 = GPIO14, D1 = GPIO15, D2 = GPIO16, D3 = GPIO17, CLK = GPIO18, CMD = GPIO19
 *
 * Current software status:
 *   - WiFi station test path is brought up through esp_hosted + esp_wifi_remote
 *   - BLE Peripheral smoke test uses hosted NimBLE over ESP-Hosted VHCI */

#define TEST_WIRELESS_SDIO_PIN_D0               GPIO_NUM_14
#define TEST_WIRELESS_SDIO_PIN_D1               GPIO_NUM_15
#define TEST_WIRELESS_SDIO_PIN_D2               GPIO_NUM_16
#define TEST_WIRELESS_SDIO_PIN_D3               GPIO_NUM_17
#define TEST_WIRELESS_SDIO_PIN_CLK              GPIO_NUM_18
#define TEST_WIRELESS_SDIO_PIN_CMD              GPIO_NUM_19

#define TEST_WIRELESS_SDIO_BUS_WIDTH            4
#define TEST_WIRELESS_SDIO_CLOCK_KHZ            25000
#define TEST_WIRELESS_CMD_TIMEOUT_MS            1000
#define TEST_WIRELESS_WIFI_SCAN_TIMEOUT_MS      10000
#define TEST_WIRELESS_WIFI_CONNECT_RETRY        3
#define TEST_WIRELESS_BLE_SCAN_TIME_MS          5000
