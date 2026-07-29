#pragma once

#include "driver/gpio.h"

namespace printsphere::board {

#if defined(PRINTSPHERE_HW_VARIANT_JC1060P470)
constexpr int kDisplayWidth = 1024;
constexpr int kDisplayHeight = 600;
constexpr gpio_num_t kI2cScl = GPIO_NUM_8;
constexpr gpio_num_t kI2cSda = GPIO_NUM_7;
constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_21;
constexpr gpio_num_t kTouchReset = GPIO_NUM_22;
constexpr int kAxp2101Address = 0;
constexpr char kBoardName[] = "JC1060P470 ESP32-P4";
#else
#error "Unknown PrintSphere hardware variant"
#endif

}  // namespace printsphere::board
