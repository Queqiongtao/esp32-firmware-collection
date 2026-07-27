#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Board contract consumed by the shared companion components and the local
 * display module (JC1060P470: MIPI-DSI JD9165 1024x600 + GT911 touch + Wi-Fi
 * through an ESP-Hosted ESP32-C6 co-processor). */

#define BOARD_TOUCH_RELEASE_EVENT_POINT_NUM UINT8_MAX
#define BOARD_DISPLAY_H_RES 1024
#define BOARD_DISPLAY_V_RES 600

/* DSI PHY power, DSI bus, JD9165 bring-up, DPI panel and backlight. */
esp_err_t board_display_init(void);
esp_err_t board_display_panel(esp_lcd_panel_handle_t *ret_panel);
esp_err_t board_backlight_set(int percent);

/* Single filtered touch point in panel coordinates. On release, pressed is 0
 * and point_num carries BOARD_TOUCH_RELEASE_EVENT_POINT_NUM together with the
 * last stable coordinates. */
esp_err_t board_touch_read(uint16_t *x, uint16_t *y, uint8_t *pressed, uint8_t *point_num);

/* Connects the ESP-Hosted transport to the C6 co-processor and waits for it
 * to come up; must run before transport_wifi_init(). */
esp_err_t board_network_init(void);

#ifdef __cplusplus
}
#endif
