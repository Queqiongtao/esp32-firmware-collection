#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error "CFG_TUSB_MCU must be defined by TinyUSB"
#endif

#define CFG_TUSB_OS OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH freertos/
#define CFG_TUD_ENABLED 1
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_VENDOR 1
#define CFG_TUD_VENDOR_RX_BUFSIZE 640
#define CFG_TUD_VENDOR_TX_BUFSIZE 64
#define CFG_TUD_VENDOR_EPSIZE 64

#ifdef __cplusplus
}
#endif
