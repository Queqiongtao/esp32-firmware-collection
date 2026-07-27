#pragma once

/* Board contract for shared companion components on the ESP32-S3 build: the
 * Waveshare AMOLED 1.8 board component already provides every required
 * symbol (board_touch_read, BOARD_TOUCH_RELEASE_EVENT_POINT_NUM), so this
 * header only re-exports it. Other boards implement the contract directly. */
#include "board_amoled_1_8.h"
