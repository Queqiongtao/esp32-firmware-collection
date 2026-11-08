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

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} clock_settings_menu_wifi_ap_t;

typedef enum {
    CLOCK_SETTINGS_MENU_SCAN_IDLE,
    CLOCK_SETTINGS_MENU_SCAN_BUSY,
    CLOCK_SETTINGS_MENU_SCAN_DONE,
    CLOCK_SETTINGS_MENU_SCAN_ERROR,
} clock_settings_menu_scan_state_t;

typedef esp_err_t (*clock_settings_menu_wifi_scan_cb_t)(void *user_ctx);
typedef clock_settings_menu_scan_state_t (*clock_settings_menu_wifi_scan_poll_cb_t)(
    clock_settings_menu_wifi_ap_t *results, uint32_t max_results,
    uint32_t *count, esp_err_t *error, void *user_ctx);

typedef struct {
    const char *name;
    const lv_image_dsc_t *preview;
} clock_settings_menu_theme_t;
typedef void (*clock_settings_menu_theme_cb_t)(uint32_t index, void *user_ctx);

typedef void (*clock_settings_menu_ntp_cb_t)(bool enabled, void *user_ctx);
typedef void (*clock_settings_menu_ntp_sync_cb_t)(void *user_ctx);
typedef void (*clock_settings_menu_close_cb_t)(void *user_ctx);
typedef void (*clock_settings_menu_close_done_cb_t)(void *user_ctx);
typedef void (*clock_settings_menu_activity_cb_t)(void *user_ctx);

typedef enum {
    CLOCK_SETTINGS_MENU_UI_SOUND_CLICK = 0,
    CLOCK_SETTINGS_MENU_UI_SOUND_CONFIRM,
    CLOCK_SETTINGS_MENU_UI_SOUND_ERROR,
    CLOCK_SETTINGS_MENU_UI_SOUND_SWIPE,
    CLOCK_SETTINGS_MENU_UI_SOUND_MENU_OPEN,
} clock_settings_menu_ui_sound_t;

typedef enum {
    CLOCK_SETTINGS_MENU_TEST_CLICK = 0,
    CLOCK_SETTINGS_MENU_TEST_CONFIRM,
    CLOCK_SETTINGS_MENU_TEST_ERROR,
    CLOCK_SETTINGS_MENU_TEST_WAKE,
    CLOCK_SETTINGS_MENU_TEST_SLEEP,
    CLOCK_SETTINGS_MENU_TEST_CHIME,
    CLOCK_SETTINGS_MENU_TEST_TIMER,
    CLOCK_SETTINGS_MENU_TEST_ALARM_CHIME,
    CLOCK_SETTINGS_MENU_TEST_ALARM_BEEP,
    CLOCK_SETTINGS_MENU_TEST_ALARM_MELODY,
    CLOCK_SETTINGS_MENU_TEST_1KHZ,
} clock_settings_menu_test_sound_t;

typedef enum {
    CLOCK_SETTINGS_MENU_TIMER_START = 0,
    CLOCK_SETTINGS_MENU_TIMER_PAUSE,
    CLOCK_SETTINGS_MENU_TIMER_STOP,
} clock_settings_menu_timer_action_t;

typedef void (*clock_settings_menu_sound_enabled_cb_t)(bool enabled, void *user_ctx);
typedef void (*clock_settings_menu_volume_cb_t)(uint8_t volume, void *user_ctx);
typedef void (*clock_settings_menu_sound_test_cb_t)(uint8_t sound, void *user_ctx);
typedef void (*clock_settings_menu_alarm_cb_t)(uint8_t index,
                                               const clock_alarm_t *alarm,
                                               void *user_ctx);
typedef void (*clock_settings_menu_timer_cb_t)(clock_settings_menu_timer_action_t action,
                                               uint32_t minutes,
                                               bool pomodoro,
                                               void *user_ctx);
typedef void (*clock_settings_menu_sleep_noise_cb_t)(bool brown,
                                                    uint32_t minutes,
                                                    bool start,
                                                    void *user_ctx);
typedef void (*clock_settings_menu_ui_sound_cb_t)(clock_settings_menu_ui_sound_t sound,
                                                  void *user_ctx);

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
    bool initialized;
    bool battery_present;
    bool charging;
    bool discharging;
    bool full;
    bool vbus_present;
    uint16_t battery_mv;
    uint8_t battery_percent;
    bool battery_percent_estimated;
} clock_settings_menu_power_status_t;

typedef struct {
    const clock_settings_t *settings;
    const clock_settings_menu_theme_t *themes;
    uint32_t theme_count;
    clock_settings_menu_theme_cb_t theme_cb;
    clock_settings_menu_brightness_cb_t brightness_cb;
    clock_settings_menu_sleep_cb_t sleep_cb;
    clock_settings_menu_auto_rotate_cb_t auto_rotate_cb;
    clock_settings_menu_wifi_cb_t wifi_cb;
    clock_settings_menu_wifi_setup_cb_t wifi_setup_cb;
    clock_settings_menu_wifi_scan_cb_t wifi_scan_cb;
    clock_settings_menu_wifi_scan_poll_cb_t wifi_scan_poll_cb;
    clock_settings_menu_ntp_cb_t ntp_cb;
    clock_settings_menu_ntp_sync_cb_t ntp_sync_cb;
    clock_settings_menu_sound_enabled_cb_t sound_enabled_cb;
    clock_settings_menu_volume_cb_t volume_cb;
    clock_settings_menu_sound_enabled_cb_t ui_sounds_cb;
    clock_settings_menu_sound_enabled_cb_t chime_cb;
    clock_settings_menu_sound_test_cb_t test_sound_cb;
    clock_settings_menu_alarm_cb_t alarm_cb;
    clock_settings_menu_timer_cb_t timer_cb;
    clock_settings_menu_sleep_noise_cb_t sleep_noise_cb;
    clock_settings_menu_ui_sound_cb_t ui_sound_cb;
    clock_settings_menu_close_cb_t close_cb;
    clock_settings_menu_activity_cb_t activity_cb;
    void *user_ctx;
} clock_settings_menu_config_t;

lv_obj_t *clock_settings_menu_create(lv_obj_t *parent, const clock_settings_menu_config_t *config);
void clock_settings_menu_move_foreground(void);
void clock_settings_menu_set_page(uint32_t page_index);
void clock_settings_menu_open_watch_faces(uint32_t page);
void clock_settings_menu_open_wifi_setup(void);
void clock_settings_menu_open_wifi_setup_edit(uint32_t field);
void clock_settings_menu_start_wifi_scan(void);
void clock_settings_menu_open_alarm_list(void);
void clock_settings_menu_open_alarm_editor(uint32_t index);
void clock_settings_menu_open_audio_test(void);
void clock_settings_menu_open_timer(void);
void clock_settings_menu_open_sleep(void);
void clock_settings_menu_update_wifi_status(const clock_settings_menu_wifi_status_t *status);
void clock_settings_menu_update_power_status(const clock_settings_menu_power_status_t *status);
void clock_settings_menu_update_timer_status(bool running,
                                             bool paused,
                                             bool pomodoro,
                                             bool break_phase,
                                             uint32_t remaining_sec);
void clock_settings_menu_update_sleep_status(bool playing, bool usb_powered);
bool clock_settings_menu_is_setup_open(void);
bool clock_settings_menu_is_power_detail_open(void);
void clock_settings_menu_open_power_detail(void);
void clock_settings_menu_close_async(clock_settings_menu_close_done_cb_t done_cb, void *user_ctx);
void clock_settings_menu_delete_immediately(void);

#ifdef __cplusplus
}
#endif

