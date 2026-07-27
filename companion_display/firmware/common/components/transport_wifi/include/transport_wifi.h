#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
struct subscreen_display;
esp_err_t transport_wifi_init(struct subscreen_display *display);
#ifdef __cplusplus
}
#endif
