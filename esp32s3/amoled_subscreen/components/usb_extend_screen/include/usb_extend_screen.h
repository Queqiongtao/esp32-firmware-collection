#pragma once

#include "esp_err.h"
#include "subscreen_display.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_extend_screen_init(subscreen_display_t *display);

#ifdef __cplusplus
}
#endif
