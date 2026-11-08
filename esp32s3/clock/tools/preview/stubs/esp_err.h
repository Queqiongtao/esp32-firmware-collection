#pragma once

// Host-side preview stub. The real ESP-IDF header is not available when
// compiling clock_settings_menu.cpp for tools/preview.
typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
