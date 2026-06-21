#pragma once

#include "clock_settings.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*clock_settings_menu_brightness_cb_t)(int brightness_percent, void *user_ctx);
typedef void (*clock_settings_menu_sleep_cb_t)(uint32_t sleep_timeout_sec, void *user_ctx);
typedef void (*clock_settings_menu_wifi_cb_t)(bool enabled, void *user_ctx);
typedef void (*clock_settings_menu_close_cb_t)(void *user_ctx);
typedef void (*clock_settings_menu_activity_cb_t)(void *user_ctx);

typedef struct {
    const clock_settings_t *settings;
    clock_settings_menu_brightness_cb_t brightness_cb;
    clock_settings_menu_sleep_cb_t sleep_cb;
    clock_settings_menu_wifi_cb_t wifi_cb;
    clock_settings_menu_close_cb_t close_cb;
    clock_settings_menu_activity_cb_t activity_cb;
    void *user_ctx;
} clock_settings_menu_config_t;

lv_obj_t *clock_settings_menu_create(lv_obj_t *parent, const clock_settings_menu_config_t *config);
void clock_settings_menu_move_foreground(void);

#ifdef __cplusplus
}
#endif

