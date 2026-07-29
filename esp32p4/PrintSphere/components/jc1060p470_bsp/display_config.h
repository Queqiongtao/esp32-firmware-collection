/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"

#define DISPLAY_PANEL_NAME                       "JD9165"
#define DISPLAY_PANEL_H_RES                      1024
#define DISPLAY_PANEL_V_RES                      600
#define DISPLAY_PANEL_BITS_PER_PIXEL             16
#define DISPLAY_PANEL_COLOR_FORMAT               LCD_COLOR_FMT_RGB565

/* Refresh rate ~= 54MHz / (1384 * 645) = 60.49Hz */
#define DISPLAY_MIPI_DSI_DPI_CLK_MHZ            54
#define DISPLAY_MIPI_DSI_LANE_NUM               2
#define DISPLAY_MIPI_DSI_LANE_BITRATE_MBPS      600
#define DISPLAY_MIPI_DSI_FRAME_BUFFER_NUM       2

#define DISPLAY_PANEL_HSYNC_PULSE_WIDTH         40
#define DISPLAY_PANEL_HSYNC_BACK_PORCH          160
#define DISPLAY_PANEL_HSYNC_FRONT_PORCH         160
#define DISPLAY_PANEL_VSYNC_PULSE_WIDTH         10
#define DISPLAY_PANEL_VSYNC_BACK_PORCH          23
#define DISPLAY_PANEL_VSYNC_FRONT_PORCH         12

#define DISPLAY_MIPI_DSI_PHY_PWR_LDO_CHAN       3
#define DISPLAY_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500

#define DISPLAY_PIN_NUM_BK_LIGHT                GPIO_NUM_23
#define DISPLAY_PIN_NUM_LCD_RST                 GPIO_NUM_27
#define DISPLAY_PIN_NUM_TOUCH_RST               GPIO_NUM_22
#define DISPLAY_PIN_NUM_TOUCH_INT               GPIO_NUM_21
#define DISPLAY_LCD_LEDC_CH                     LEDC_CHANNEL_0
#define DISPLAY_LCD_LEDC_TIMER                  LEDC_TIMER_1
#define DISPLAY_LCD_LEDC_MODE                   LEDC_LOW_SPEED_MODE

#define DISPLAY_TOUCH_NATIVE_H_RES              800
#define DISPLAY_TOUCH_NATIVE_V_RES              480

#define DISPLAY_LVGL_DRAW_BUF_LINES             CONFIG_EXAMPLE_LVGL_DRAW_BUF_LINES
#define DISPLAY_LVGL_TICK_PERIOD_MS             2
#define DISPLAY_LVGL_TASK_STACK_SIZE            (32 * 1024)
#define DISPLAY_LVGL_TASK_PRIORITY              5
#define DISPLAY_LVGL_TASK_CORE_ID               0
#define DISPLAY_LVGL_TASK_MAX_DELAY_MS          500
#define DISPLAY_LVGL_TASK_MIN_DELAY_MS          (1000 / CONFIG_FREERTOS_HZ)

#if CONFIG_EXAMPLE_LVGL_DRAW_BUF_INTERNAL
#define DISPLAY_LVGL_DRAW_BUF_CAPS              (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define DISPLAY_LVGL_DRAW_BUF_MEM_NAME          "internal RAM"
#else
#define DISPLAY_LVGL_DRAW_BUF_CAPS              MALLOC_CAP_SPIRAM
#define DISPLAY_LVGL_DRAW_BUF_MEM_NAME          "PSRAM"
#endif

#define DISPLAY_ALIGN_UP(num, align)            (((num) + ((align) - 1)) & ~((align) - 1))
#define DISPLAY_ALIGN_DOWN(num, align)          ((num) & ~((align) - 1))
