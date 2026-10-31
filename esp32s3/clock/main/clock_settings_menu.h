#pragma once

#include <stdint.h>

#include "clock_settings.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*clock_settings_menu_brightness_cb_t)(int brightness_percent, void *user_ctx);
typedef void (*clock_settings_menu_sleep_cb_t)(uint32_t sleep_timeout_sec, void *user_ctx);
typedef void (*clock_settings_menu_auto_rotate_cb_t)(bool enabled, void *user_ctx);
typedef void (*clock_settings_menu_wifi_cb_t)(bool enabled, void *user_ctx);
typedef bool (*clock_settings_menu_wifi_setup_cb_t)(const char *ssid, const char *password, void *user_ctx);
typedef void (*clock_settings_menu_ntp_cb_t)(bool enabled, void *user_ctx);
typedef void (*clock_settings_menu_ntp_sync_cb_t)(void *user_ctx);
typedef void (*clock_settings_menu_close_cb_t)(void *user_ctx);
typedef void (*clock_settings_menu_close_done_cb_t)(void *user_ctx);
typedef void (*clock_settings_menu_activity_cb_t)(void *user_ctx);

typedef struct {
    const char *state;
    const char *detail;
    const char *ssid;
    bool enabled;
    bool has_credentials;
    bool connected;
    bool ntp_enabled;
} clock_settings_menu_wifi_status_t;

typedef struct {
    const clock_settings_t *settings;
    clock_settings_menu_brightness_cb_t brightness_cb;
    clock_settings_menu_sleep_cb_t sleep_cb;
    clock_settings_menu_auto_rotate_cb_t auto_rotate_cb;
    clock_settings_menu_wifi_cb_t wifi_cb;
    clock_settings_menu_wifi_setup_cb_t wifi_setup_cb;
    clock_settings_menu_ntp_cb_t ntp_cb;
    clock_settings_menu_ntp_sync_cb_t ntp_sync_cb;
    clock_settings_menu_close_cb_t close_cb;
    clock_settings_menu_activity_cb_t activity_cb;
    void *user_ctx;
} clock_settings_menu_config_t;

lv_obj_t *clock_settings_menu_create(lv_obj_t *parent, const clock_settings_menu_config_t *config);
void clock_settings_menu_move_foreground(void);
void clock_settings_menu_set_page(uint32_t page_index);
void clock_settings_menu_open_wifi_setup(void);
void clock_settings_menu_open_wifi_setup_edit(uint32_t field);
void clock_settings_menu_update_wifi_status(const clock_settings_menu_wifi_status_t *status);
bool clock_settings_menu_is_setup_open(void);
void clock_settings_menu_close_async(clock_settings_menu_close_done_cb_t done_cb, void *user_ctx);
void clock_settings_menu_delete_immediately(void);

#ifdef __cplusplus
}
#endif

