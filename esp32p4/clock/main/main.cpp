/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "audio_manager.h"
#include "bsp/esp-bsp.h"
#include "button_gpio.h"
#include "clock_settings.h"
#include "clock_alert_scheduler.h"
#include "clock_settings_menu.h"
#include "clock_touch_gesture.h"
#include "clock_theme.h"
#include "clock_theme_previews.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "iot_button.h"
#include "lvgl.h"
#include "ntp_time_sync.h"
#include "nvs_flash.h"
#include "rtc_time_sync.h"
#include "sdkconfig.h"
#include "wifi_manager.h"

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
#include "esp_lv_adapter.h"
#endif

static const char *TAG = "ClockDemo";

LV_FONT_DECLARE(lv_font_ui_62);

static const clock_theme_t *const s_themes[] = {
    &g_clock_theme_minimal,
    &g_clock_theme_segment,
    &g_clock_theme_analog,
    &g_clock_theme_m5_classic,
    &g_clock_theme_m5_simple,
    &g_clock_theme_m5_big_number,
    &g_clock_theme_m5_number_flow,
    &g_clock_theme_dot_matrix,
};
static constexpr size_t s_theme_count = sizeof(s_themes) / sizeof(s_themes[0]);
static size_t s_current_theme_index = 0;
static int s_selected_theme_pending = -1;
static_assert(s_theme_count == 8, "Update the watch-face preview catalog when adding themes");

static constexpr gpio_num_t CLOCK_BUTTON_GPIO = GPIO_NUM_35;
static constexpr uint8_t CLOCK_BUTTON_ACTIVE_LEVEL = 0;
static constexpr int32_t CLOCK_DISPLAY_SIZE = 800;
static constexpr int32_t CLOCK_UI_SIZE = CLOCK_DISPLAY_SIZE;

static constexpr uint32_t CLOCK_INPUT_TIMER_MS = 15;
static constexpr uint32_t CLOCK_SETTINGS_SAVE_DELAY_MS = 500;
static constexpr uint32_t CLOCK_MENU_TIMEOUT_MS = 60 * 1000;
static constexpr uint32_t CLOCK_THEME_ANIMATION_TIMER_MS = 33;
static constexpr uint32_t CLOCK_THEME_TRANSITION_TIMER_MS = 16;
static constexpr uint32_t CLOCK_THEME_TRANSITION_OUT_MS = 70;
static constexpr uint32_t CLOCK_THEME_TRANSITION_IN_MS = 90;
static constexpr int32_t CLOCK_THEME_TRANSITION_SCALE_MIN = 244;
static constexpr int32_t CLOCK_THEME_TRANSITION_SCALE_MAX = 256;
static constexpr uint32_t CLOCK_BRIGHTNESS_FADE_TIMER_MS = 16;
static constexpr uint32_t CLOCK_BRIGHTNESS_SLEEP_FADE_MS = 220;
static constexpr uint32_t CLOCK_BRIGHTNESS_WAKE_FADE_MS = 240;
static lv_display_t *s_display = NULL;
static lv_obj_t *s_ui_root = NULL;
static clock_settings_t s_settings = {};
static lv_obj_t *s_current_theme_root = NULL;
static lv_obj_t *s_clock_snapshot_image = NULL;
static lv_obj_t *s_settings_menu = NULL;
static lv_draw_buf_t *s_clock_snapshot = NULL;
static lv_timer_t *s_clock_update_timer = NULL;
static lv_timer_t *s_theme_animation_timer = NULL;
static lv_timer_t *s_auto_sleep_timer = NULL;
static lv_timer_t *s_wifi_status_timer = NULL;
static uint32_t s_ntp_manual_sync_until_ms = 0;
static lv_timer_t *s_input_timer = NULL;
static lv_timer_t *s_settings_save_timer = NULL;
static lv_timer_t *s_theme_transition_timer = NULL;
static lv_timer_t *s_brightness_fade_timer = NULL;
static bool s_clock_snapshot_dirty = false;
static bool s_settings_save_pending = false;
static volatile bool s_clock_rendering_enabled = true;
static volatile int s_theme_switch_pending_step = 0;
static volatile bool s_button_single_click_pending = false;
static volatile bool s_menu_toggle_pending = false;
static uint32_t s_last_activity_ms = 0;
static uint32_t s_settings_menu_last_activity_ms = 0;
static bool s_settings_menu_closing = false;
static bool s_touch_action_blocked_until_release = false;
static bool s_screen_sleeping = false;
enum class ThemeTransitionPhase : uint8_t {
    Idle,
    Out,
    In,
};
static ThemeTransitionPhase s_theme_transition_phase = ThemeTransitionPhase::Idle;
static uint32_t s_theme_transition_start_ms = 0;
static uint32_t s_theme_transition_duration_ms = 0;
static int32_t s_theme_transition_from_scale = CLOCK_THEME_TRANSITION_SCALE_MAX;
static int32_t s_theme_transition_to_scale = CLOCK_THEME_TRANSITION_SCALE_MAX;
static lv_opa_t s_theme_transition_from_opa = LV_OPA_COVER;
static lv_opa_t s_theme_transition_to_opa = LV_OPA_COVER;

static bool s_brightness_fade_active = false;
static uint32_t s_brightness_fade_start_ms = 0;
static uint32_t s_brightness_fade_duration_ms = 0;
static int s_brightness_fade_from = 0;
static int s_brightness_fade_to = 0;
static int s_brightness_current_percent = 0;
static bool s_brightness_current_valid = false;

enum class AlertKind : uint8_t {
    None,
    Alarm,
    Timer,
};

static lv_timer_t *s_audio_event_timer = NULL;
static lv_obj_t *s_alert_overlay = NULL;
static AlertKind s_alert_kind = AlertKind::None;
static uint64_t s_alert_started_ms = 0;
static int s_alarm_active_index = -1;
static ClockAlertScheduler s_alert_scheduler;
static bool s_timer_running = false;
static bool s_timer_paused = false;
static bool s_timer_pomodoro = false;
static bool s_timer_break = false;
static uint32_t s_timer_total_ms = 0;
static uint32_t s_timer_remaining_ms = 0;
static uint64_t s_timer_end_ms = 0;
static uint32_t s_timer_selected_minutes = 25;
static bool s_sleep_noise_active = false;
static uint64_t s_sleep_noise_end_ms = 0;
static bool s_vbus_present = true;

static void keep_touch_layer_foreground(void);
static void close_settings_menu(void);
static void close_settings_menu_immediately(void);
static void open_settings_menu(void);
static bool note_user_activity(void);
static void play_wake_sound_if_enabled(void);
static void play_sleep_sound_if_enabled(void);
static bool sleep_screen(const char *reason);
static void update_clock_rendering_state(void);
static void update_auto_sleep(lv_timer_t *timer);
static void update_inputs(lv_timer_t *timer);
static void update_clock(lv_timer_t *timer);
static void update_theme_animation(lv_timer_t *timer);
static void update_theme_transition(lv_timer_t *timer);
static void update_brightness_fade(lv_timer_t *timer);
static void update_settings_menu_timeout(lv_timer_t *timer);
static void update_network_status(lv_timer_t *timer);
static void update_audio_events(lv_timer_t *timer);
static void show_alarm_overlay(void);
static void show_timer_done_overlay(void);
static void close_alert_overlay(void);
static void trigger_alarm(int index);
static void snooze_alarm(void);
static void stop_alarm(void);
static void stop_timer(void);
static void cancel_theme_transition(void);

static const clock_theme_t *current_theme(void)
{
    return s_themes[s_current_theme_index];
}

static void update_clock_rendering_state(void)
{
    const bool enabled = !s_screen_sleeping && s_settings_menu == NULL;
    s_clock_rendering_enabled = enabled;

    if (!enabled) {
        if (s_clock_update_timer != NULL) {
            lv_timer_pause(s_clock_update_timer);
        }
        if (s_theme_animation_timer != NULL) {
            lv_timer_pause(s_theme_animation_timer);
        }
        return;
    }

    if (s_clock_update_timer != NULL) {
        lv_timer_resume(s_clock_update_timer);
    }

    if (s_theme_animation_timer != NULL) {
        if (current_theme()->tick != NULL) {
            lv_timer_resume(s_theme_animation_timer);
        } else {
            lv_timer_pause(s_theme_animation_timer);
        }
    }

    update_clock(NULL);
}

static void set_current_theme_overlay_visible(bool visible)
{
    visible = visible && s_theme_transition_phase == ThemeTransitionPhase::Idle;
    const clock_theme_t *theme = current_theme();
    if (theme->set_overlay_visible != NULL) {
        theme->set_overlay_visible(visible);
    }
}

static void destroy_current_theme_overlay(void)
{
    const clock_theme_t *theme = current_theme();
    if (theme->destroy_overlay != NULL) {
        theme->destroy_overlay();
    }
}

static void refresh_clock_snapshot(void)
{
    if (s_current_theme_root == NULL || s_clock_snapshot_image == NULL) {
        return;
    }

    lv_obj_clear_flag(s_current_theme_root, LV_OBJ_FLAG_HIDDEN);
    set_current_theme_overlay_visible(false);
    lv_obj_add_flag(s_clock_snapshot_image, LV_OBJ_FLAG_HIDDEN);

    // Snapshot the full screen so the theme and its overlay (for example the
    // M5 Simple seconds dot) are captured at the native 800x800 resolution.
    lv_obj_t *snapshot_source = lv_screen_active();
    lv_draw_buf_t *snapshot = s_clock_snapshot;
    if (snapshot == NULL) {
        snapshot = lv_snapshot_create_draw_buf(snapshot_source, LV_COLOR_FORMAT_RGB565);
        s_clock_snapshot = snapshot;
    }

    lv_result_t snapshot_result = LV_RESULT_INVALID;
    if (snapshot != NULL) {
        snapshot_result =
            lv_snapshot_take_to_draw_buf(snapshot_source, LV_COLOR_FORMAT_RGB565, snapshot);
    }

    lv_draw_buf_t *old_snapshot = s_clock_snapshot;
    if (snapshot_result != LV_RESULT_OK) {
        snapshot = lv_snapshot_take(snapshot_source, LV_COLOR_FORMAT_RGB565);
        if (snapshot != NULL) {
            s_clock_snapshot = snapshot;
        }
    }

    lv_obj_add_flag(s_current_theme_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_clock_snapshot_image, LV_OBJ_FLAG_HIDDEN);
    set_current_theme_overlay_visible(true);

    if (snapshot == NULL) {
        ESP_LOGW(TAG, "Failed to refresh clock snapshot");
        s_clock_snapshot_dirty = true;
        // Do not leave a blank screen behind when both the reusable buffer and
        // the fallback allocation fail. Show the live theme tree instead.
        lv_obj_add_flag(s_clock_snapshot_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_current_theme_root, LV_OBJ_FLAG_HIDDEN);
        set_current_theme_overlay_visible(true);
        keep_touch_layer_foreground();
        return;
    }

    lv_image_set_src(s_clock_snapshot_image, s_clock_snapshot);

    const int32_t image_width = lv_image_get_src_width(s_clock_snapshot_image);
    const int32_t image_height = lv_image_get_src_height(s_clock_snapshot_image);
    lv_image_set_pivot(s_clock_snapshot_image, image_width / 2, image_height / 2);
    lv_obj_center(s_clock_snapshot_image);

    if (old_snapshot != NULL && old_snapshot != s_clock_snapshot) {
        lv_draw_buf_destroy(old_snapshot);
    }

    s_clock_snapshot_dirty = false;
    keep_touch_layer_foreground();
}

static void switch_theme(int step);
static void reset_current_theme(void);

static void keep_touch_layer_foreground(void)
{
    if (s_alert_overlay != NULL) {
        lv_obj_move_foreground(s_alert_overlay);
    } else if (s_settings_menu != NULL) {
        clock_settings_menu_move_foreground();
    } else {
        clock_touch_gesture_move_foreground();
    }
}

static void request_theme_switch(int step)
{
    if (step == 0) {
        return;
    }

    // Accumulate requests while a transition is running so fast swipes are not lost.
    s_theme_switch_pending_step += step;
}

static uint32_t clock_animation_progress(uint32_t elapsed_ms, uint32_t duration_ms)
{
    if (duration_ms == 0 || elapsed_ms >= duration_ms) {
        return 1000;
    }
    return (uint32_t)((uint64_t)elapsed_ms * 1000U / duration_ms);
}

static int32_t clock_animation_lerp_i32(int32_t from, int32_t to, uint32_t progress)
{
    return from + (int32_t)(((int64_t)(to - from) * (int32_t)progress) / 1000);
}

static lv_opa_t clock_animation_lerp_opa(lv_opa_t from, lv_opa_t to, uint32_t progress)
{
    return (lv_opa_t)clock_animation_lerp_i32((int32_t)from, (int32_t)to, progress);
}

static void set_theme_transition_visual(int32_t scale, lv_opa_t opa)
{
    if (s_clock_snapshot_image == NULL) {
        return;
    }

    lv_image_set_scale(s_clock_snapshot_image, (uint32_t)scale);
    lv_obj_set_style_opa(s_clock_snapshot_image, opa, 0);
}

static void setup_theme_transition_phase(ThemeTransitionPhase phase,
                                         uint32_t duration_ms,
                                         int32_t from_scale,
                                         int32_t to_scale,
                                         lv_opa_t from_opa,
                                         lv_opa_t to_opa)
{
    s_theme_transition_phase = phase;
    s_theme_transition_start_ms = lv_tick_get();
    s_theme_transition_duration_ms = duration_ms;
    s_theme_transition_from_scale = from_scale;
    s_theme_transition_to_scale = to_scale;
    s_theme_transition_from_opa = from_opa;
    s_theme_transition_to_opa = to_opa;
    set_theme_transition_visual(from_scale, from_opa);
}

static void start_theme_transition_out(void)
{
    if (s_theme_transition_phase != ThemeTransitionPhase::Idle) {
        return;
    }

    if (s_clock_snapshot_image == NULL || s_theme_transition_timer == NULL) {
        const int steps = s_theme_switch_pending_step;
        s_theme_switch_pending_step = 0;
        if (steps != 0) {
            switch_theme(steps);
            if (s_clock_snapshot_dirty) {
                refresh_clock_snapshot();
            }
        }
        return;
    }

    set_current_theme_overlay_visible(false);
    setup_theme_transition_phase(ThemeTransitionPhase::Out,
                                 CLOCK_THEME_TRANSITION_OUT_MS,
                                 CLOCK_THEME_TRANSITION_SCALE_MAX,
                                 CLOCK_THEME_TRANSITION_SCALE_MIN,
                                 LV_OPA_COVER,
                                 LV_OPA_TRANSP);
    lv_timer_reset(s_theme_transition_timer);
    lv_timer_resume(s_theme_transition_timer);
}

static void update_theme_transition(lv_timer_t *timer)
{
    if (s_theme_transition_phase == ThemeTransitionPhase::Idle) {
        lv_timer_pause(timer);
        return;
    }

    const uint32_t now_ms = lv_tick_get();
    const uint32_t progress = clock_animation_progress(now_ms - s_theme_transition_start_ms,
                                                       s_theme_transition_duration_ms);
    set_theme_transition_visual(
        clock_animation_lerp_i32(s_theme_transition_from_scale,
                                 s_theme_transition_to_scale,
                                 progress),
        clock_animation_lerp_opa(s_theme_transition_from_opa,
                                 s_theme_transition_to_opa,
                                 progress));

    if (progress < 1000) {
        return;
    }

    if (s_theme_transition_phase == ThemeTransitionPhase::Out) {
        const int steps = s_theme_switch_pending_step;
        s_theme_switch_pending_step = 0;
        if (steps != 0) {
            switch_theme(steps);
            if (s_clock_snapshot_dirty) {
                refresh_clock_snapshot();
            }
        }

        setup_theme_transition_phase(ThemeTransitionPhase::In,
                                     CLOCK_THEME_TRANSITION_IN_MS,
                                     CLOCK_THEME_TRANSITION_SCALE_MIN,
                                     CLOCK_THEME_TRANSITION_SCALE_MAX,
                                     LV_OPA_TRANSP,
                                     LV_OPA_COVER);
        return;
    }

    set_theme_transition_visual(CLOCK_THEME_TRANSITION_SCALE_MAX, LV_OPA_COVER);
    s_theme_transition_phase = ThemeTransitionPhase::Idle;
    set_current_theme_overlay_visible(true);

    if (s_clock_snapshot_dirty) {
        refresh_clock_snapshot();
    }

    if (s_theme_switch_pending_step != 0) {
        start_theme_transition_out();
    } else {
        lv_timer_pause(timer);
    }
}

static void cancel_theme_transition(void)
{
    const int steps = s_theme_switch_pending_step;
    s_theme_switch_pending_step = 0;
    s_theme_transition_phase = ThemeTransitionPhase::Idle;
    set_current_theme_overlay_visible(true);
    if (s_theme_transition_timer != NULL) {
        lv_timer_pause(s_theme_transition_timer);
    }

    if (steps != 0) {
        switch_theme(steps);
        if (s_clock_snapshot_dirty) {
            refresh_clock_snapshot();
        }
    }

    set_theme_transition_visual(CLOCK_THEME_TRANSITION_SCALE_MAX, LV_OPA_COVER);
}

static void reset_current_theme(void)
{
    const clock_theme_t *theme = current_theme();
    if (theme->reset != NULL) {
        theme->reset();
    }
}

static void save_settings_now(void)
{
    esp_err_t err = clock_settings_save(&s_settings);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Save settings failed: %s", esp_err_to_name(err));
    }
}

static void on_settings_save_timer(lv_timer_t *timer)
{
    (void)timer;
    if (!s_settings_save_pending) {
        return;
    }

    s_settings_save_pending = false;
    save_settings_now();
}

static void schedule_settings_save(void)
{
    s_settings_save_pending = true;
    if (s_settings_save_timer == NULL) {
        s_settings_save_pending = false;
        save_settings_now();
        return;
    }

    lv_timer_set_repeat_count(s_settings_save_timer, 1);
    lv_timer_reset(s_settings_save_timer);
    lv_timer_resume(s_settings_save_timer);
}

static void flush_settings_save(void)
{
    if (!s_settings_save_pending) {
        return;
    }

    s_settings_save_pending = false;
    if (s_settings_save_timer != NULL) {
        lv_timer_pause(s_settings_save_timer);
        lv_timer_set_repeat_count(s_settings_save_timer, -1);
    }
    save_settings_now();
}

static void apply_brightness(int brightness_percent)
{
    if (brightness_percent < 0) {
        brightness_percent = 0;
    } else if (brightness_percent > 100) {
        brightness_percent = 100;
    }

    if (s_brightness_current_valid && s_brightness_current_percent == brightness_percent) {
        return;
    }

    esp_err_t err = bsp_display_brightness_set(brightness_percent);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Set brightness %d%% failed: %s",
                 brightness_percent,
                 esp_err_to_name(err));
        return;
    }

    s_brightness_current_percent = brightness_percent;
    s_brightness_current_valid = true;
}

static void cancel_brightness_fade(void)
{
    s_brightness_fade_active = false;
    if (s_brightness_fade_timer != NULL) {
        lv_timer_pause(s_brightness_fade_timer);
    }
}

static void start_brightness_fade(int brightness_percent, uint32_t duration_ms)
{
    if (brightness_percent < 0) {
        brightness_percent = 0;
    } else if (brightness_percent > 100) {
        brightness_percent = 100;
    }

    if (s_brightness_fade_timer == NULL) {
        apply_brightness(brightness_percent);
        return;
    }

    const int from_percent =
        s_brightness_current_valid ? s_brightness_current_percent : brightness_percent;
    if (from_percent == brightness_percent || duration_ms == 0) {
        s_brightness_fade_active = false;
        lv_timer_pause(s_brightness_fade_timer);
        apply_brightness(brightness_percent);
        return;
    }

    s_brightness_fade_active = true;
    s_brightness_fade_from = from_percent;
    s_brightness_fade_to = brightness_percent;
    s_brightness_fade_start_ms = lv_tick_get();
    s_brightness_fade_duration_ms = duration_ms;
    lv_timer_reset(s_brightness_fade_timer);
    lv_timer_resume(s_brightness_fade_timer);
}

static void update_brightness_fade(lv_timer_t *timer)
{
    if (!s_brightness_fade_active) {
        lv_timer_pause(timer);
        return;
    }

    const uint32_t now_ms = lv_tick_get();
    const uint32_t progress = clock_animation_progress(now_ms - s_brightness_fade_start_ms,
                                                       s_brightness_fade_duration_ms);
    const int brightness_percent =
        clock_animation_lerp_i32(s_brightness_fade_from, s_brightness_fade_to, progress);
    apply_brightness(brightness_percent);

    if (progress < 1000) {
        return;
    }

    apply_brightness(s_brightness_fade_to);
    s_brightness_fade_active = false;
    lv_timer_pause(timer);
}

static bool is_touch_action_blocked(void)
{
    return s_touch_action_blocked_until_release;
}

static bool wake_screen_if_sleeping(void)
{
    if (!s_screen_sleeping) {
        return false;
    }

    s_screen_sleeping = false;
    s_last_activity_ms = lv_tick_get();
    update_clock_rendering_state();
    start_brightness_fade(s_settings.brightness_percent, CLOCK_BRIGHTNESS_WAKE_FADE_MS);
    return true;
}

static bool sleep_screen(const char *reason)
{
    if (s_screen_sleeping) {
        return false;
    }

    s_screen_sleeping = true;
    cancel_theme_transition();
    if (s_settings_menu != NULL) {
        close_settings_menu_immediately();
    }
    update_clock_rendering_state();
    start_brightness_fade(0, CLOCK_BRIGHTNESS_SLEEP_FADE_MS);
    play_sleep_sound_if_enabled();
    ESP_LOGI(TAG, "screen %s sleep", reason);
    return true;
}

static void play_wake_sound_if_enabled(void)
{
    if (!s_settings.sound_enabled || !s_settings.ui_sounds ||
        s_alert_kind != AlertKind::None || audio_manager_is_noise_playing()) {
        return;
    }
    audio_manager_play(AUDIO_SOUND_WAKE, false);
}

static void play_sleep_sound_if_enabled(void)
{
    if (!s_settings.sound_enabled || !s_settings.ui_sounds ||
        s_alert_kind != AlertKind::None || audio_manager_is_noise_playing()) {
        return;
    }
    audio_manager_play(AUDIO_SOUND_SLEEP, false);
}

static bool note_user_activity(void)
{
    const bool woke = wake_screen_if_sleeping();
    s_last_activity_ms = lv_tick_get();
    if (woke) {
        s_touch_action_blocked_until_release = true;
        play_wake_sound_if_enabled();
    }
    return woke;
}

static void on_menu_activity(void *user_ctx)
{
    (void)user_ctx;
    s_settings_menu_last_activity_ms = lv_tick_get();
    note_user_activity();
}

static void on_menu_brightness(int brightness_percent, void *user_ctx)
{
    (void)user_ctx;
    s_settings.brightness_percent = brightness_percent;
    if (!s_screen_sleeping) {
        cancel_brightness_fade();
        apply_brightness(brightness_percent);
    }
    schedule_settings_save();
}

static void on_menu_sleep(uint32_t sleep_timeout_sec, void *user_ctx)
{
    (void)user_ctx;
    s_settings.sleep_timeout_sec = sleep_timeout_sec;
    schedule_settings_save();
}

static void on_menu_wifi(bool enabled, void *user_ctx)
{
    (void)user_ctx;

    if (enabled && !wifi_manager_has_credentials()) {
        s_settings.wifi_enabled = false;
        schedule_settings_save();
        return;
    }

    const bool previous_enabled = s_settings.wifi_enabled;
    s_settings.wifi_enabled = enabled;
    if (!enabled) {
        s_ntp_manual_sync_until_ms = 0;
    }
    const esp_err_t err = wifi_manager_set_enabled(enabled);
    if (err != ESP_OK) {
        s_settings.wifi_enabled = previous_enabled;
        ESP_LOGW(TAG, "Set WiFi enabled=%d failed: %s", enabled, esp_err_to_name(err));
    }
    schedule_settings_save();
    ESP_LOGI(TAG, "WiFi setting is %s", enabled ? "on" : "off");
}

static esp_err_t on_menu_wifi_scan(void *user_ctx)
{
    (void)user_ctx;
    if (wifi_manager_scan_results(nullptr, 0, nullptr, nullptr) == WIFI_MANAGER_SCAN_BUSY) {
        return ESP_OK; // Reopened UI attaches to the existing request.
    }
    return wifi_manager_scan_start();
}

static clock_settings_menu_scan_state_t on_menu_wifi_scan_poll(
    clock_settings_menu_wifi_ap_t *results, uint32_t max_results,
    uint32_t *count, esp_err_t *error, void *user_ctx)
{
    (void)user_ctx;
    wifi_manager_ap_t aps[8] = {};
    size_t found = 0;
    const auto state = wifi_manager_scan_results(aps, max_results < 8 ? max_results : 8,
                                                  &found, error);
    *count = static_cast<uint32_t>(found);
    for (size_t i = 0; i < found; ++i) {
        snprintf(results[i].ssid, sizeof(results[i].ssid), "%s", aps[i].ssid);
        results[i].rssi = aps[i].rssi;
        results[i].secure = aps[i].secure;
    }
    switch (state) {
    case WIFI_MANAGER_SCAN_BUSY: return CLOCK_SETTINGS_MENU_SCAN_BUSY;
    case WIFI_MANAGER_SCAN_DONE: return CLOCK_SETTINGS_MENU_SCAN_DONE;
    case WIFI_MANAGER_SCAN_ERROR: return CLOCK_SETTINGS_MENU_SCAN_ERROR;
    default: return CLOCK_SETTINGS_MENU_SCAN_IDLE;
    }
}

static bool on_menu_wifi_setup(const char *ssid, const char *password, void *user_ctx)
{
    (void)user_ctx;

    ESP_LOGI(TAG,
             "WiFi setup request: ssid=%s password=%s",
             ssid,
             password != nullptr ? "provided" : "unchanged");
    const esp_err_t err = wifi_manager_connect(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connect request failed: %s", esp_err_to_name(err));
        return false;
    }

    s_settings.wifi_enabled = true;
    schedule_settings_save();
    ESP_LOGI(TAG, "WiFi credentials saved for SSID=%s", ssid);
    return true;
}

static void on_menu_ntp(bool enabled, void *user_ctx)
{
    (void)user_ctx;
    s_settings.ntp_enabled = enabled;
    if (enabled) {
        wifi_manager_status_t status = {};
        wifi_manager_get_status(&status);
        if (status.state == WIFI_MANAGER_CONNECTED) {
            ntp_time_sync_sync_now();
        }
    } else {
        s_ntp_manual_sync_until_ms = 0;
        ntp_time_sync_stop();
    }
    schedule_settings_save();
    ESP_LOGI(TAG, "NTP auto sync is %s", enabled ? "on" : "off");
}

static void on_menu_ntp_sync(void *user_ctx)
{
    (void)user_ctx;
    wifi_manager_status_t status = {};
    wifi_manager_get_status(&status);
    if (status.state != WIFI_MANAGER_CONNECTED) {
        return;
    }
    const esp_err_t err = ntp_time_sync_sync_now();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Manual NTP sync failed: %s", esp_err_to_name(err));
        return;
    }

    // Keep SNTP alive long enough to finish a one-shot sync even when Auto NTP
    // is disabled; update_network_status() stops it after this window.
    s_ntp_manual_sync_until_ms = lv_tick_get() + 30000;
}


static void on_menu_sound_enabled(bool enabled, void *user_ctx)
{
    (void)user_ctx;
    s_settings.sound_enabled = enabled;
    if (!enabled && s_alarm_active_index < 0) {
        audio_manager_stop();
        s_sleep_noise_active = false;
        s_sleep_noise_end_ms = 0;
    }
    schedule_settings_save();
    ESP_LOGI(TAG, "Sound is %s", enabled ? "on" : "off");
}

static void on_menu_volume(uint8_t volume, void *user_ctx)
{
    (void)user_ctx;
    s_settings.sound_volume = volume;
    audio_manager_set_volume(volume);
    schedule_settings_save();
}

static void on_menu_ui_sounds(bool enabled, void *user_ctx)
{
    (void)user_ctx;
    s_settings.ui_sounds = enabled;
    schedule_settings_save();
}

static void on_menu_chime(bool enabled, void *user_ctx)
{
    (void)user_ctx;
    s_settings.hourly_chime = enabled;
    schedule_settings_save();
}

static void on_menu_test_sound(uint8_t sound, void *user_ctx)
{
    (void)user_ctx;
    if (!s_settings.sound_enabled || s_alert_kind != AlertKind::None) {
        return;
    }
    if (audio_manager_is_noise_playing()) {
        audio_manager_stop();
    }

    audio_manager_sound_t selected = AUDIO_SOUND_CONFIRM;
    switch (sound) {
    case CLOCK_SETTINGS_MENU_TEST_CLICK:
        selected = AUDIO_SOUND_CLICK;
        break;
    case CLOCK_SETTINGS_MENU_TEST_CONFIRM:
        selected = AUDIO_SOUND_CONFIRM;
        break;
    case CLOCK_SETTINGS_MENU_TEST_ERROR:
        selected = AUDIO_SOUND_ERROR;
        break;
    case CLOCK_SETTINGS_MENU_TEST_WAKE:
        selected = AUDIO_SOUND_WAKE;
        break;
    case CLOCK_SETTINGS_MENU_TEST_SLEEP:
        selected = AUDIO_SOUND_SLEEP;
        break;
    case CLOCK_SETTINGS_MENU_TEST_CHIME:
        selected = AUDIO_SOUND_CHIME;
        break;
    case CLOCK_SETTINGS_MENU_TEST_TIMER:
        selected = AUDIO_SOUND_TIMER_DONE;
        break;
    case CLOCK_SETTINGS_MENU_TEST_ALARM_CHIME:
        selected = AUDIO_SOUND_ALARM_CHIME;
        break;
    case CLOCK_SETTINGS_MENU_TEST_ALARM_BEEP:
        selected = AUDIO_SOUND_ALARM_BEEP;
        break;
    case CLOCK_SETTINGS_MENU_TEST_ALARM_MELODY:
        selected = AUDIO_SOUND_ALARM_MELODY;
        break;
    case CLOCK_SETTINGS_MENU_TEST_1KHZ:
    default:
        selected = AUDIO_SOUND_TEST;
        break;
    }
    audio_manager_play(selected, false);
}

static void on_menu_alarm(uint8_t index, const clock_alarm_t *alarm, void *user_ctx)
{
    (void)user_ctx;
    if (index >= CLOCK_ALARM_COUNT || alarm == nullptr) {
        return;
    }
    const auto &previous = s_settings.alarms[index];
    if (previous.hour != alarm->hour || previous.minute != alarm->minute ||
        previous.weekdays != alarm->weekdays || previous.enabled != alarm->enabled) {
        s_alert_scheduler.reconfigure(index);
    }
    s_settings.alarms[index] = *alarm;
    schedule_settings_save();
    ESP_LOGI(TAG,
             "Alarm %u: %02u:%02u enabled=%d weekdays=0x%02x sound=%u",
             (unsigned)(index + 1),
             alarm->hour,
             alarm->minute,
             alarm->enabled ? 1 : 0,
             alarm->weekdays,
             alarm->sound);
}

static void on_menu_ui_sound(clock_settings_menu_ui_sound_t sound, void *user_ctx)
{
    (void)user_ctx;
    if (!s_settings.sound_enabled || !s_settings.ui_sounds ||
        s_alert_kind != AlertKind::None || audio_manager_is_noise_playing()) {
        return;
    }

    audio_manager_sound_t selected = AUDIO_SOUND_CLICK;
    switch (sound) {
    case CLOCK_SETTINGS_MENU_UI_SOUND_CONFIRM:
        selected = AUDIO_SOUND_CONFIRM;
        break;
    case CLOCK_SETTINGS_MENU_UI_SOUND_ERROR:
        selected = AUDIO_SOUND_ERROR;
        break;
    case CLOCK_SETTINGS_MENU_UI_SOUND_SWIPE:
        selected = AUDIO_SOUND_SWIPE;
        break;
    case CLOCK_SETTINGS_MENU_UI_SOUND_MENU_OPEN:
        selected = AUDIO_SOUND_MENU_OPEN;
        break;
    case CLOCK_SETTINGS_MENU_UI_SOUND_CLICK:
    default:
        selected = AUDIO_SOUND_CLICK;
        break;
    }
    audio_manager_play(selected, false);
}

static lv_obj_t *create_alert_shell(const char *title, const char *big_text, const char *detail)
{
    lv_obj_t *overlay = lv_obj_create(s_ui_root != NULL ? s_ui_root : lv_screen_active());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, CLOCK_UI_SIZE, CLOCK_UI_SIZE);
    lv_obj_center(overlay);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(overlay, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);

    lv_obj_t *heading = lv_label_create(overlay);
    lv_label_set_text(heading, title);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_42, 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(0xF5F3FF), 0);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, clock_theme_px(32));

    if (big_text != nullptr) {
        lv_obj_t *big = lv_label_create(overlay);
        lv_label_set_text(big, big_text);
        lv_obj_set_style_text_font(big, &lv_font_ui_62, 0);
        lv_obj_set_style_text_color(big, lv_color_hex(0xF5F3FF), 0);
        lv_obj_align(big, LV_ALIGN_TOP_MID, 0, clock_theme_px(92));
    }

    if (detail != nullptr) {
        lv_obj_t *detail_label = lv_label_create(overlay);
        lv_label_set_text(detail_label, detail);
        lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_34, 0);
        lv_obj_set_style_text_color(detail_label, lv_color_hex(0x8B86A3), 0);
        lv_obj_align(detail_label, LV_ALIGN_TOP_MID, 0, clock_theme_px(168));
    }

    s_alert_overlay = overlay;
    lv_obj_move_foreground(overlay);
    return overlay;
}

static lv_obj_t *create_alert_button(lv_obj_t *overlay,
                                     const char *text,
                                     int32_t x,
                                     int32_t y,
                                     lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(overlay);
    lv_obj_set_size(button, clock_theme_px(160), clock_theme_px(48));
    lv_obj_align(button, LV_ALIGN_TOP_MID, clock_theme_px(x), clock_theme_px(y));
    lv_obj_set_style_radius(button, clock_theme_px(24), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x241B3A), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2E2348), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, clock_theme_px(1), 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x5EEAD4), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x5EEAD4), 0);
    lv_obj_center(label);
    return button;
}

static void close_alert_overlay(void)
{
    if (s_alert_overlay != nullptr) {
        lv_obj_delete(s_alert_overlay);
    }
    s_alert_overlay = nullptr;
    s_alert_kind = AlertKind::None;
    clock_touch_gesture_set_enabled(true);
    keep_touch_layer_foreground();
}

static void show_alarm_overlay(void)
{
    if (s_alarm_active_index < 0 || s_alarm_active_index >= CLOCK_ALARM_COUNT) {
        return;
    }
    if (s_alert_overlay != nullptr) {
        close_alert_overlay();
    }
    wake_screen_if_sleeping();
    if (s_settings_menu != nullptr) {
        close_settings_menu_immediately();
    }

    char time_text[16] = {};
    const clock_alarm_t &alarm = s_settings.alarms[s_alarm_active_index];
    snprintf(time_text, sizeof(time_text), "%02u:%02u", alarm.hour, alarm.minute);
    char detail[32] = {};
    snprintf(detail, sizeof(detail), "Alarm %u", (unsigned)(s_alarm_active_index + 1));
    create_alert_shell("ALARM", time_text, detail);
    create_alert_button(s_alert_overlay, "Snooze 5m", -86, 250,
                        [](lv_event_t *event) {
                            (void)event;
                            snooze_alarm();
                        });
    create_alert_button(s_alert_overlay, "Stop", 86, 250,
                        [](lv_event_t *event) {
                            (void)event;
                            stop_alarm();
                        });
    s_alert_kind = AlertKind::Alarm;
    s_alert_started_ms = esp_timer_get_time() / 1000;
    clock_touch_gesture_set_enabled(false);
    lv_obj_move_foreground(s_alert_overlay);
}

static void show_timer_done_overlay(void)
{
    if (s_alarm_active_index >= 0) {
        return;
    }
    if (s_alert_overlay != nullptr) {
        close_alert_overlay();
    }
    wake_screen_if_sleeping();
    if (s_settings_menu != nullptr) {
        close_settings_menu_immediately();
    }
    create_alert_shell("TIMER", "00:00", "Time is up");
    create_alert_button(s_alert_overlay, "OK", 0, 250,
                        [](lv_event_t *event) {
                            (void)event;
                            audio_manager_stop();
                            close_alert_overlay();
                            note_user_activity();
                        });
    s_alert_kind = AlertKind::Timer;
    s_alert_started_ms = esp_timer_get_time() / 1000;
    clock_touch_gesture_set_enabled(false);
    lv_obj_move_foreground(s_alert_overlay);
}

static void trigger_alarm(int index)
{
    if (index < 0 || index >= CLOCK_ALARM_COUNT) {
        return;
    }
    s_alarm_active_index = index;
    if (s_alert_kind == AlertKind::Timer) {
        s_alert_scheduler.complete_timer();
    }
    s_sleep_noise_active = false;
    s_sleep_noise_end_ms = 0;
    show_alarm_overlay();
    // The master sound switch controls UI/chime/timer/noise. Alarms keep
    // their own ring path so a muted UI does not silently skip an alarm.
    audio_manager_play_alarm(s_settings.alarms[index].sound);
    ESP_LOGI(TAG, "Alarm %d triggered", index + 1);
}

static void snooze_alarm(void)
{
    if (s_alarm_active_index < 0) {
        return;
    }
    s_alert_scheduler.snooze(s_alarm_active_index, esp_timer_get_time() / 1000);
    s_alarm_active_index = -1;
    audio_manager_stop();
    close_alert_overlay();
    note_user_activity();
    ESP_LOGI(TAG, "Alarm snoozed for 5 minutes");
}

static void stop_alarm(void)
{
    if (s_alarm_active_index >= 0) {
        const int index = s_alarm_active_index;
        s_alert_scheduler.cancel(index);
        if (s_settings.alarms[index].weekdays == 0) {
            s_settings.alarms[index].enabled = false;
            schedule_settings_save();
        }
    }
    s_alarm_active_index = -1;

    audio_manager_stop();
    close_alert_overlay();
    note_user_activity();
}

static void stop_timer(void)
{
    s_alert_scheduler.cancel_timer();
    s_timer_running = false;
    s_timer_paused = false;
    s_timer_remaining_ms = 0;
    s_timer_end_ms = 0;
    if (s_alert_kind == AlertKind::Timer) {
        close_alert_overlay();
    }
    if (s_alarm_active_index < 0 && !s_sleep_noise_active) {
        audio_manager_stop();
    }
    clock_settings_menu_update_timer_status(false, false, s_timer_pomodoro, false, 0);
}

static void on_menu_timer(clock_settings_menu_timer_action_t action,
                          uint32_t minutes,
                          bool pomodoro,
                          void *user_ctx)
{
    (void)user_ctx;
    const uint64_t now_ms = esp_timer_get_time() / 1000;
    if (action == CLOCK_SETTINGS_MENU_TIMER_STOP) {
        stop_timer();
        return;
    }
    if (action == CLOCK_SETTINGS_MENU_TIMER_PAUSE) {
        if (s_timer_running && !s_timer_paused) {
            const uint64_t remaining = s_timer_end_ms > now_ms ? s_timer_end_ms - now_ms : 0;
            s_timer_remaining_ms = (uint32_t)remaining;
            s_timer_paused = true;
        }
        clock_settings_menu_update_timer_status(s_timer_running, s_timer_paused,
                                                s_timer_pomodoro, s_timer_break,
                                                (s_timer_remaining_ms + 999) / 1000);
        return;
    }

    if (s_timer_paused && s_timer_remaining_ms > 0) {
        s_timer_end_ms = now_ms + s_timer_remaining_ms;
        s_timer_paused = false;
        s_timer_running = true;
    } else {
        s_timer_selected_minutes = minutes;
        s_timer_pomodoro = pomodoro;
        s_timer_break = false;
        s_timer_total_ms = (pomodoro ? 25U : minutes) * 60U * 1000U;
        s_timer_remaining_ms = s_timer_total_ms;
        s_timer_end_ms = now_ms + s_timer_total_ms;
        s_timer_running = true;
        s_timer_paused = false;
    }
    clock_settings_menu_update_timer_status(s_timer_running, s_timer_paused,
                                            s_timer_pomodoro, s_timer_break,
                                            (s_timer_remaining_ms + 999) / 1000);
}

static void on_menu_sleep_noise(bool brown, uint32_t minutes, bool start, void *user_ctx)
{
    (void)user_ctx;
    if (!start) {
        s_sleep_noise_active = false;
        s_sleep_noise_end_ms = 0;
        if (s_alarm_active_index < 0) {
            audio_manager_stop();
        }
        clock_settings_menu_update_sleep_status(false, s_vbus_present);
        return;
    }

    if (!s_settings.sound_enabled || !s_vbus_present) {
        if (s_settings.sound_enabled) {
            audio_manager_play(AUDIO_SOUND_ERROR, false);
        }
        clock_settings_menu_update_sleep_status(false, s_vbus_present);
        return;
    }

    const uint64_t now_ms = esp_timer_get_time() / 1000;
    s_sleep_noise_active = true;
    s_sleep_noise_end_ms = now_ms + (uint64_t)minutes * 60ULL * 1000ULL;
    audio_manager_start_noise(brown, minutes * 60U * 1000U);
    clock_settings_menu_update_sleep_status(true, s_vbus_present);
}

static void check_alarms(const struct tm &timeinfo)
{
    const uint64_t now_ms = esp_timer_get_time() / 1000;
    s_alert_scheduler.collect(timeinfo, s_settings.alarms, now_ms);
    if (s_alarm_active_index >= 0 &&
        ClockAlertScheduler::expired(s_alert_started_ms, now_ms)) {
        stop_alarm();
    }
    if (s_alarm_active_index < 0) {
        const int next = s_alert_scheduler.take_alarm();
        if (next >= 0) {
            trigger_alarm(next);
        }
    }
}

static void update_timer_event(const struct tm &timeinfo)
{
    (void)timeinfo;
    const uint64_t now_ms = esp_timer_get_time() / 1000;

    if (s_timer_running && !s_timer_paused) {
        if (now_ms >= s_timer_end_ms) {
            if (s_timer_pomodoro) {
                s_timer_break = !s_timer_break;
                const uint32_t minutes = s_timer_break ? 5U : 25U;
                s_timer_total_ms = minutes * 60U * 1000U;
                s_timer_end_ms = now_ms + s_timer_total_ms;
                s_timer_remaining_ms = s_timer_total_ms;
                if (s_alert_kind == AlertKind::None && s_settings.sound_enabled &&
                    !audio_manager_is_noise_playing()) {
                    audio_manager_play(AUDIO_SOUND_CHIME, false);
                }
            } else {
                s_timer_running = false;
                s_timer_paused = false;
                s_timer_remaining_ms = 0;
                s_timer_end_ms = 0;
                s_alert_scheduler.complete_timer();
            }
        } else {
            s_timer_remaining_ms = (uint32_t)(s_timer_end_ms - now_ms);
        }
    }

    clock_settings_menu_update_timer_status(s_timer_running,
                                            s_timer_paused,
                                            s_timer_pomodoro,
                                            s_timer_break,
                                            (s_timer_remaining_ms + 999U) / 1000U);
}

static void update_chime_event(const struct tm &timeinfo)
{
    const bool allowed = s_settings.sound_enabled && s_settings.hourly_chime &&
                         s_alert_kind == AlertKind::None && !s_screen_sleeping &&
                         !audio_manager_is_noise_playing();
    if (s_alert_scheduler.take_chime(timeinfo, allowed)) {
        audio_manager_play(AUDIO_SOUND_CHIME, false);
    }
}

static void update_audio_events(lv_timer_t *timer)
{
    (void)timer;

    time_t now = 0;
    struct tm timeinfo = {};
    time(&now);
    localtime_r(&now, &timeinfo);

    check_alarms(timeinfo);
    update_timer_event(timeinfo);
    if (s_alert_scheduler.take_timer(s_alarm_active_index >= 0)) {
        s_sleep_noise_active = false;
        s_sleep_noise_end_ms = 0;
        audio_manager_stop();
        if (s_settings.sound_enabled) {
            audio_manager_play(AUDIO_SOUND_TIMER_DONE, false);
        }
        show_timer_done_overlay();
    }
    update_chime_event(timeinfo);

    const uint64_t now_ms = esp_timer_get_time() / 1000;
    if (s_alert_kind == AlertKind::Timer && s_alert_overlay != nullptr &&
        ClockAlertScheduler::expired(s_alert_started_ms, now_ms)) {
        // A finished timer must not keep the screen awake forever.
        close_alert_overlay();
    }

    if (s_sleep_noise_active &&
        (!s_vbus_present || now_ms >= s_sleep_noise_end_ms ||
         !audio_manager_is_noise_playing())) {
        s_sleep_noise_active = false;
        s_sleep_noise_end_ms = 0;
        if (s_alarm_active_index < 0) {
            audio_manager_stop();
        }
    }
    clock_settings_menu_update_sleep_status(s_sleep_noise_active, s_vbus_present);
}

static void on_menu_theme(uint32_t index, void *user_ctx)
{
    (void)user_ctx;
    if (index < s_theme_count && !s_settings_menu_closing) {
        s_selected_theme_pending = static_cast<int>(index);
        close_settings_menu();
    }
}

static void on_menu_close(void *user_ctx)
{
    (void)user_ctx;
    close_settings_menu();
}

static void open_settings_menu(void)
{
    if (s_alert_kind != AlertKind::None) {
        return;
    }
    if (s_settings_menu != NULL) {
        keep_touch_layer_foreground();
        return;
    }

    cancel_theme_transition();
    cancel_brightness_fade();
    if (!s_screen_sleeping) {
        apply_brightness(s_settings.brightness_percent);
    }

    lv_obj_t *screen = s_ui_root != NULL ? s_ui_root : lv_screen_active();
    clock_settings_menu_config_t menu_config = {
        .settings = &s_settings,
        .themes = g_clock_theme_previews,
        .theme_count = s_theme_count,
        .theme_cb = on_menu_theme,
        .brightness_cb = on_menu_brightness,
        .sleep_cb = on_menu_sleep,
        .wifi_cb = on_menu_wifi,
        .wifi_setup_cb = on_menu_wifi_setup,
        .wifi_scan_cb = on_menu_wifi_scan,
        .wifi_scan_poll_cb = on_menu_wifi_scan_poll,
        .ntp_cb = on_menu_ntp,
        .ntp_sync_cb = on_menu_ntp_sync,
        .sound_enabled_cb = on_menu_sound_enabled,
        .volume_cb = on_menu_volume,
        .ui_sounds_cb = on_menu_ui_sounds,
        .chime_cb = on_menu_chime,
        .test_sound_cb = on_menu_test_sound,
        .alarm_cb = on_menu_alarm,
        .timer_cb = on_menu_timer,
        .sleep_noise_cb = on_menu_sleep_noise,
        .ui_sound_cb = on_menu_ui_sound,
        .close_cb = on_menu_close,
        .activity_cb = on_menu_activity,
        .user_ctx = NULL,
    };
    s_settings_menu = clock_settings_menu_create(screen, &menu_config);
    if (s_settings_menu == NULL) {
        ESP_LOGW(TAG, "Failed to create settings menu");
        return;
    }
    if (s_settings.sound_enabled && s_settings.ui_sounds &&
        s_alert_kind == AlertKind::None && !audio_manager_is_noise_playing()) {
        audio_manager_play(AUDIO_SOUND_MENU_OPEN, false);
    }

    s_settings_menu_closing = false;
    s_settings_menu_last_activity_ms = lv_tick_get();
    update_network_status(NULL);
    clock_touch_gesture_set_enabled(false);
    keep_touch_layer_foreground();
    update_clock_rendering_state();
}

static void on_settings_menu_closed(void *user_ctx)
{
    (void)user_ctx;
    s_settings_menu = NULL;
    s_settings_menu_closing = false;
    clock_touch_gesture_set_enabled(true);
    keep_touch_layer_foreground();
    update_clock_rendering_state();
    if (s_selected_theme_pending >= 0) {
        const int step = s_selected_theme_pending - static_cast<int>(s_current_theme_index);
        s_selected_theme_pending = -1;
        if (step != 0) {
            request_theme_switch(step);
        }
    }
}

static void close_settings_menu(void)
{
    if (s_settings_menu == NULL || s_settings_menu_closing) {
        return;
    }

    s_settings_menu_closing = true;
    flush_settings_save();
    clock_settings_menu_close_async(on_settings_menu_closed, NULL);
}

static void close_settings_menu_immediately(void)
{
    if (s_settings_menu == NULL) {
        return;
    }

    s_selected_theme_pending = -1;
    flush_settings_save();
    clock_settings_menu_delete_immediately();
    s_settings_menu = NULL;
    s_settings_menu_closing = false;
    clock_touch_gesture_set_enabled(true);
    keep_touch_layer_foreground();
    update_clock_rendering_state();
}

static void update_settings_menu_timeout(lv_timer_t *timer)
{
    (void)timer;

    if (s_settings_menu == NULL || s_settings_menu_closing || s_screen_sleeping) {
        return;
    }

    const uint32_t now_ms = lv_tick_get();
    if ((uint32_t)(now_ms - s_settings_menu_last_activity_ms) < CLOCK_MENU_TIMEOUT_MS) {
        return;
    }

    ESP_LOGI(TAG, "settings menu auto close after %" PRIu32 " ms", CLOCK_MENU_TIMEOUT_MS);
    close_settings_menu();
}

static const char *wifi_reason_text(uint8_t reason)
{
    if (reason == 0) {
        return "Connection failed";
    }
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "Network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "Wrong password";
    case WIFI_REASON_ASSOC_FAIL:
        return "Association failed";
    case WIFI_REASON_CONNECTION_FAIL:
        return "Connection failed";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "Signal lost";
    default:
        return "Connection lost";
    }
}

static void update_network_status(lv_timer_t *timer)
{
    (void)timer;

    ntp_time_sync_update();

    wifi_manager_status_t wifi = {};
    wifi_manager_get_status(&wifi);
    const bool connected = wifi.state == WIFI_MANAGER_CONNECTED;

    const uint32_t now_ms = lv_tick_get();
    const bool manual_ntp_active =
        s_ntp_manual_sync_until_ms != 0 &&
        (int32_t)(s_ntp_manual_sync_until_ms - now_ms) > 0;

    if (connected && (s_settings.ntp_enabled || manual_ntp_active)) {
        if (!ntp_time_sync_is_running() &&
            (manual_ntp_active ||
             (!ntp_time_sync_is_retry_waiting() && ntp_time_sync_is_due()))) {
            ntp_time_sync_start();
        }
    } else {
        ntp_time_sync_stop();
    }

    if (ntp_time_sync_take_rtc_write_pending()) {
        const esp_err_t err = rtc_time_sync_write_from_system();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to commit internal RTC time after NTP sync: %s", esp_err_to_name(err));
        }
    }

    if (s_settings_menu == NULL) {
        return;
    }

    char state[64] = {};
    char detail[72] = {};
    switch (wifi.state) {
    case WIFI_MANAGER_OFF:
        snprintf(state, sizeof(state), "WiFi off");
        snprintf(detail,
                 sizeof(detail),
                 "%s",
                 wifi.has_credentials ? "Tap switch to connect" : "Tap Setup to configure");
        break;
    case WIFI_MANAGER_CONNECTING:
        snprintf(state, sizeof(state), "Connecting...");
        snprintf(detail, sizeof(detail), "%s", wifi.ssid[0] != '\0' ? wifi.ssid : "Starting WiFi...");
        break;
    case WIFI_MANAGER_CONNECTED: {
        snprintf(state, sizeof(state), "Connected: %s", wifi.ssid);

        char network[40] = {};
        if (wifi.ip[0] != '\0' && wifi.rssi_valid) {
            snprintf(network, sizeof(network), "IP %s  %d dBm", wifi.ip, wifi.rssi);
        } else if (wifi.ip[0] != '\0') {
            snprintf(network, sizeof(network), "IP %s", wifi.ip);
        } else {
            snprintf(network, sizeof(network), "Connected");
        }

        char ntp[24] = {};
        if (ntp_time_sync_is_syncing()) {
            snprintf(ntp, sizeof(ntp), "Syncing time...");
        } else if (ntp_time_sync_is_retry_waiting()) {
            const uint32_t retry_ms = ntp_time_sync_retry_in_ms();
            const uint32_t seconds = (retry_ms + 999U) / 1000U;
            if (seconds >= 60U) {
                snprintf(ntp, sizeof(ntp), "Retry %u min", (seconds + 59U) / 60U);
            } else {
                snprintf(ntp, sizeof(ntp), "Retry %u s", seconds);
            }
        } else if (!s_settings.ntp_enabled && !manual_ntp_active) {
            snprintf(ntp, sizeof(ntp), "NTP off");
        } else {
            const time_t last_sync = ntp_time_sync_last_sync_time();
            if (last_sync > 0) {
                struct tm timeinfo = {};
                localtime_r(&last_sync, &timeinfo);
                snprintf(ntp, sizeof(ntp), "Sync %02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
            } else {
                snprintf(ntp, sizeof(ntp), "Not synced");
            }
        }
        snprintf(detail, sizeof(detail), "%s | %s", network, ntp);
        break;
    }
    case WIFI_MANAGER_FAILED: {
        char retry[20] = {};
        if (wifi.retry_in_ms > 0) {
            const uint32_t seconds = (wifi.retry_in_ms + 999U) / 1000U;
            if (seconds >= 60U) {
                snprintf(retry, sizeof(retry), "%u min", (seconds + 59U) / 60U);
            } else {
                snprintf(retry, sizeof(retry), "%u s", seconds);
            }
            snprintf(detail,
                     sizeof(detail),
                     "%s | Retry in %s",
                     wifi_reason_text(wifi.disconnect_reason),
                     retry);
        } else {
            snprintf(detail,
                     sizeof(detail),
                     "%s | Retrying",
                     wifi_reason_text(wifi.disconnect_reason));
        }
        snprintf(state, sizeof(state), "Reconnecting...");
        break;
    }
    default:
        snprintf(state, sizeof(state), "WiFi unknown");
        break;
    }

    clock_settings_menu_wifi_status_t status = {};
    status.state = state;
    status.detail = detail;
    status.ssid = wifi.ssid;
    status.enabled = wifi.enabled;
    status.has_credentials = wifi.has_credentials;
    status.connected = connected;
    status.ntp_enabled = s_settings.ntp_enabled;
    clock_settings_menu_update_wifi_status(&status);
}

static void toggle_settings_menu(void)
{
    if (s_settings_menu != NULL) {
        close_settings_menu();
    } else {
        open_settings_menu();
    }
}

static void process_pending_input_actions(void)
{
    const bool button_single_click = s_button_single_click_pending;
    const bool menu_toggle = s_menu_toggle_pending;
    s_button_single_click_pending = false;
    s_menu_toggle_pending = false;

    if (button_single_click) {
        if (note_user_activity()) {
            return;
        }
        if (s_settings_menu != NULL) {
            close_settings_menu();
        } else {
            request_theme_switch(1);
        }
    }

    if (menu_toggle) {
        if (note_user_activity()) {
            return;
        }
        toggle_settings_menu();
    }
}

static void update_auto_sleep(lv_timer_t *timer)
{
    (void)timer;

    if (s_screen_sleeping || s_alert_overlay != NULL ||
        s_settings.sleep_timeout_sec == 0 || s_last_activity_ms == 0) {
        return;
    }

    const uint32_t now_ms = lv_tick_get();
    const uint32_t timeout_ms = s_settings.sleep_timeout_sec * 1000;
    if ((uint32_t)(now_ms - s_last_activity_ms) < timeout_ms) {
        return;
    }

    if (sleep_screen("auto")) {
        ESP_LOGI(TAG, "screen auto sleep after %" PRIu32 " seconds", s_settings.sleep_timeout_sec);
    }
}

static void update_inputs(lv_timer_t *timer)
{
    (void)timer;

    process_pending_input_actions();

    if (s_theme_switch_pending_step != 0 &&
        s_theme_transition_phase == ThemeTransitionPhase::Idle) {
        start_theme_transition_out();
    }
}

static void update_clock(lv_timer_t *timer)
{
    (void)timer;

    if (!s_clock_rendering_enabled || s_screen_sleeping || s_settings_menu != NULL) {
        return;
    }

    time_t now = 0;
    struct tm timeinfo = {};

    time(&now);
    localtime_r(&now, &timeinfo);

    if (s_themes[s_current_theme_index]->update(&timeinfo)) {
        s_clock_snapshot_dirty = true;
    }

    if (s_theme_transition_phase == ThemeTransitionPhase::Idle &&
        s_clock_snapshot_dirty) {
        refresh_clock_snapshot();
    }
}

static void update_theme_animation(lv_timer_t *timer)
{
    (void)timer;

    if (!s_clock_rendering_enabled || s_screen_sleeping || s_settings_menu != NULL) {
        return;
    }

    const clock_theme_t *theme = current_theme();
    if (theme->tick == NULL) {
        return;
    }
    if (theme->tick(lv_tick_get())) {
        if (theme->tick_updates_overlay_only == NULL || !theme->tick_updates_overlay_only()) {
            s_clock_snapshot_dirty = true;
            if (s_theme_transition_phase == ThemeTransitionPhase::Idle) {
                refresh_clock_snapshot();
            }
        }
    }
}

static void switch_theme(int step)
{
    if (s_theme_count <= 1 || s_current_theme_root == NULL) {
        return;
    }

    lv_obj_t *screen = s_ui_root != NULL ? s_ui_root : lv_screen_active();
    set_current_theme_overlay_visible(false);
    destroy_current_theme_overlay();
    lv_obj_del(s_current_theme_root);

    const int normalized_step = step % (int)s_theme_count;
    s_current_theme_index =
        (s_current_theme_index + s_theme_count + normalized_step) % s_theme_count;
    s_current_theme_root = s_themes[s_current_theme_index]->build(screen);
    s_settings.theme_index = (uint8_t)s_current_theme_index;
    schedule_settings_save();
    ESP_LOGI(TAG, "switch theme -> %s", s_themes[s_current_theme_index]->name);
    if (s_settings.sound_enabled && s_settings.ui_sounds &&
        s_alert_kind == AlertKind::None && !audio_manager_is_noise_playing()) {
        audio_manager_play(AUDIO_SOUND_CLICK, false);
    }

    reset_current_theme();
    s_clock_snapshot_dirty = true;
    update_clock(NULL);
    if (s_theme_animation_timer != NULL && s_clock_rendering_enabled) {
        if (s_themes[s_current_theme_index]->tick != NULL) {
            lv_timer_resume(s_theme_animation_timer);
        } else {
            lv_timer_pause(s_theme_animation_timer);
        }
    }
    keep_touch_layer_foreground();
}

static void on_touch_theme_switch(int step, void *user_ctx)
{
    (void)user_ctx;
    if (is_touch_action_blocked()) {
        return;
    }
    if (note_user_activity()) {
        return;
    }
    request_theme_switch(step);
}

static void on_touch_long_press(void *user_ctx)
{
    (void)user_ctx;
    if (is_touch_action_blocked()) {
        return;
    }
    if (note_user_activity()) {
        return;
    }
    toggle_settings_menu();
}

static void on_touch_activity(lv_event_code_t code, void *user_ctx)
{
    (void)user_ctx;
    if (code == LV_EVENT_RELEASED) {
        note_user_activity();
        s_touch_action_blocked_until_release = false;
        return;
    }
    note_user_activity();
}

static void create_clock_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // Snapshot image is a sibling below the native 800x800 UI root. Keeping it
    // on the screen lets theme transitions cover the full panel.
    s_clock_snapshot_image = lv_image_create(screen);
    lv_obj_center(s_clock_snapshot_image);
    lv_obj_add_flag(s_clock_snapshot_image, LV_OBJ_FLAG_HIDDEN);
    s_clock_snapshot =
        lv_draw_buf_create(CLOCK_DISPLAY_SIZE,
                           CLOCK_DISPLAY_SIZE,
                           LV_COLOR_FORMAT_RGB565,
                           LV_STRIDE_AUTO);
    if (s_clock_snapshot == NULL) {
        ESP_LOGW(TAG, "Failed to allocate reusable clock snapshot buffer");
    }

    // Themes, menu and overlays all build directly in 800x800 coordinates.
    s_ui_root = lv_obj_create(screen);
    lv_obj_remove_style_all(s_ui_root);
    lv_obj_clear_flag(s_ui_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui_root, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_size(s_ui_root, CLOCK_UI_SIZE, CLOCK_UI_SIZE);
    lv_obj_center(s_ui_root);

    s_current_theme_root = s_themes[s_current_theme_index]->build(s_ui_root);
    reset_current_theme();

    clock_touch_gesture_create(s_ui_root,
                               bsp_display_get_input_dev(),
                               on_touch_theme_switch,
                               NULL,
                               on_touch_long_press,
                               NULL,
                               on_touch_activity,
                               NULL);

    s_last_activity_ms = lv_tick_get();
    s_clock_snapshot_dirty = true;
    s_clock_update_timer = lv_timer_create(update_clock, 1000, NULL);
    s_theme_animation_timer =
        lv_timer_create(update_theme_animation, CLOCK_THEME_ANIMATION_TIMER_MS, NULL);
    s_auto_sleep_timer = lv_timer_create(update_auto_sleep, 1000, NULL);
    lv_timer_create(update_settings_menu_timeout, 1000, NULL);
    s_wifi_status_timer = lv_timer_create(update_network_status, 500, NULL);
    s_audio_event_timer = lv_timer_create(update_audio_events, 1000, NULL);
    s_input_timer = lv_timer_create(update_inputs, CLOCK_INPUT_TIMER_MS, NULL);

    s_theme_transition_timer =
        lv_timer_create(update_theme_transition, CLOCK_THEME_TRANSITION_TIMER_MS, NULL);
    if (s_theme_transition_timer != NULL) {
        lv_timer_set_auto_delete(s_theme_transition_timer, false);
        lv_timer_set_repeat_count(s_theme_transition_timer, -1);
        lv_timer_pause(s_theme_transition_timer);
    }

    s_brightness_fade_timer =
        lv_timer_create(update_brightness_fade, CLOCK_BRIGHTNESS_FADE_TIMER_MS, NULL);
    if (s_brightness_fade_timer != NULL) {
        lv_timer_set_auto_delete(s_brightness_fade_timer, false);
        lv_timer_set_repeat_count(s_brightness_fade_timer, -1);
        lv_timer_pause(s_brightness_fade_timer);
    }

    s_settings_save_timer =
        lv_timer_create(on_settings_save_timer, CLOCK_SETTINGS_SAVE_DELAY_MS, NULL);
    if (s_settings_save_timer != NULL) {
        lv_timer_set_auto_delete(s_settings_save_timer, false);
        lv_timer_set_repeat_count(s_settings_save_timer, -1);
        lv_timer_pause(s_settings_save_timer);
    }

    update_clock_rendering_state();
    keep_touch_layer_foreground();
}

static void on_theme_button(void *button_handle, void *user_ctx)
{
    (void)button_handle;
    (void)user_ctx;
    s_button_single_click_pending = true;
}

static void on_menu_button(void *button_handle, void *user_ctx)
{
    (void)button_handle;
    (void)user_ctx;
    s_menu_toggle_pending = true;
}

static EventGroupHandle_t s_hosted_events = nullptr;
static constexpr EventBits_t HOSTED_UP_BIT = BIT0;
static constexpr EventBits_t HOSTED_FAILURE_BIT = BIT1;
static constexpr uint32_t HOSTED_READY_TIMEOUT_MS = 10000;

static void hosted_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (s_hosted_events == nullptr) {
        return;
    }
    if (id == ESP_HOSTED_EVENT_TRANSPORT_UP) {
        xEventGroupSetBits(s_hosted_events, HOSTED_UP_BIT);
    } else if (id == ESP_HOSTED_EVENT_TRANSPORT_DOWN ||
               id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE) {
        xEventGroupSetBits(s_hosted_events, HOSTED_FAILURE_BIT);
    }
}

static esp_err_t p4_network_init(void)
{
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Create default event loop failed: %s", esp_err_to_name(err));
        return err;
    }

    if (s_hosted_events == nullptr) {
        s_hosted_events = xEventGroupCreate();
        if (s_hosted_events == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        err = esp_event_handler_instance_register(ESP_HOSTED_EVENT,
                                                  ESP_EVENT_ANY_ID,
                                                  hosted_event_handler,
                                                  nullptr,
                                                  nullptr);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Register ESP-Hosted event failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    xEventGroupClearBits(s_hosted_events, HOSTED_UP_BIT | HOSTED_FAILURE_BIT);
    ESP_LOGI(TAG, "Connecting ESP-Hosted transport to the on-board ESP32-C6");
    const int hosted_err = esp_hosted_connect_to_slave();
    if (hosted_err != 0) {
        ESP_LOGW(TAG, "esp_hosted_connect_to_slave failed: %d", hosted_err);
        return ESP_FAIL;
    }

    const EventBits_t bits = xEventGroupWaitBits(s_hosted_events,
                                                 HOSTED_UP_BIT | HOSTED_FAILURE_BIT,
                                                 pdFALSE,
                                                 pdFALSE,
                                                 pdMS_TO_TICKS(HOSTED_READY_TIMEOUT_MS));
    if ((bits & HOSTED_FAILURE_BIT) != 0) {
        ESP_LOGW(TAG, "ESP-Hosted transport reported a failure");
        return ESP_FAIL;
    }
    if ((bits & HOSTED_UP_BIT) == 0) {
        ESP_LOGW(TAG, "ESP-Hosted transport did not become ready in %u ms",
                 (unsigned)HOSTED_READY_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "ESP-Hosted transport is ready");
    return ESP_OK;
}

static void log_wifi_sta_mac(void)
{
    uint8_t mac[6] = {};
    const esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Read STA MAC failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG,
             "WiFi STA MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void clock_button_init(void)
{
    button_config_t btn_cfg = {};
    button_gpio_config_t gpio_cfg = {
        .gpio_num = CLOCK_BUTTON_GPIO,
        .active_level = CLOCK_BUTTON_ACTIVE_LEVEL,
    };

    button_handle_t btn = NULL;
    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Button init failed: %s", esp_err_to_name(err));
        return;
    }

    err = iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, on_theme_button, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Button cb register failed: %s", esp_err_to_name(err));
    }

    err = iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, on_menu_button, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Button long press cb register failed: %s", esp_err_to_name(err));
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Start ESP32-P4 clock demo");

    setenv("TZ", "CST-8", 1);
    tzset();

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    esp_err_t settings_err = clock_settings_load(&s_settings);
    if (settings_err != ESP_OK) {
        ESP_LOGW(TAG, "Load settings failed: %s", esp_err_to_name(settings_err));
        clock_settings_defaults(&s_settings);
    }
    s_current_theme_index = (size_t)s_settings.theme_index;
    if (s_current_theme_index >= s_theme_count) {
        s_current_theme_index = 0;
        s_settings.theme_index = 0;
    }
    ESP_LOGI(TAG,
             "restore theme -> %s (index=%u)",
             s_themes[s_current_theme_index]->name,
             (unsigned)s_current_theme_index);

    rtc_time_sync_from_external_rtc();

    const esp_err_t audio_err = audio_manager_init();
    if (audio_err == ESP_OK) {
        audio_manager_set_volume(s_settings.sound_volume);
        const esp_err_t audio_prepare_err = audio_manager_prepare();
        if (audio_prepare_err != ESP_OK) {
            ESP_LOGW(TAG, "Audio codec prepare failed: %s", esp_err_to_name(audio_prepare_err));
        }
        ESP_LOGI(TAG,
                 "audio ready: sound=%s volume=%u ui=%s chime=%s",
                 s_settings.sound_enabled ? "on" : "off",
                 (unsigned)s_settings.sound_volume,
                 s_settings.ui_sounds ? "on" : "off",
                 s_settings.hourly_chime ? "on" : "off");
    } else {
        ESP_LOGW(TAG, "Audio manager init failed: %s", esp_err_to_name(audio_err));
    }

    bsp_display_cfg_t display_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    // The settings menu creates several full-size overlays from LVGL event
    // callbacks. 16 KB avoids stack overflows seen on the 8 KB default when
    // opening the 11-button Audio test page.
    display_cfg.lv_adapter_cfg.task_stack_size = 16 * 1024;
    s_display = bsp_display_start_with_config(&display_cfg);
    ESP_ERROR_CHECK(s_display ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(bsp_display_brightness_set(s_settings.brightness_percent));
    s_brightness_current_percent = s_settings.brightness_percent;
    s_brightness_current_valid = true;
    ESP_LOGI(TAG, "touch indev: %p", bsp_display_get_input_dev());

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
    ESP_ERROR_CHECK(esp_lv_adapter_fps_stats_enable(s_display, true));
#endif

    ESP_ERROR_CHECK(bsp_display_lock(UINT32_MAX));
    create_clock_ui();
    bsp_display_unlock();

    clock_button_init();

    // Bring up the display and clock before the optional network path. Hosted
    // transport discovery can take a few seconds when the C6 is absent.
    const esp_err_t network_err = p4_network_init();
    if (network_err != ESP_OK) {
        ESP_LOGW(TAG, "ESP-Hosted init failed: %s; WiFi may stay unavailable",
                 esp_err_to_name(network_err));
    }

    esp_err_t wifi_err = wifi_manager_init();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi init failed: %s", esp_err_to_name(wifi_err));
    } else {
        log_wifi_sta_mac();
        ntp_time_sync_init();
        if (s_settings.wifi_enabled) {
            if (wifi_manager_has_credentials()) {
                wifi_manager_set_enabled(true);
            } else {
                s_settings.wifi_enabled = false;
                clock_settings_save(&s_settings);
            }
        }
    }

}
