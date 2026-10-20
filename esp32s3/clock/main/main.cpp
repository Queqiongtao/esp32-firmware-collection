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

#include "bsp/esp-bsp.h"
#include "button_gpio.h"
#include "clock_settings.h"
#include "clock_settings_menu.h"
#include "clock_touch_gesture.h"
#include "clock_theme.h"
#include "esp_log.h"
#include "esp_io_expander.h"
#include "freertos/FreeRTOS.h"
#include "imu_demo.h"
#include "iot_button.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "rtc_time_sync.h"
#include "sdkconfig.h"
#include "spring_angle.h"

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
#include "esp_lv_adapter.h"
#endif

static const char *TAG = "ClockDemo";

static const clock_theme_t *const s_themes[] = {
    &g_clock_theme_minimal,
    &g_clock_theme_segment,
    &g_clock_theme_analog,
    &g_clock_theme_outline_analog,
    &g_clock_theme_mooncat,
    &g_clock_theme_m5_classic,
    &g_clock_theme_m5_simple,
    &g_clock_theme_m5_big_number,
    &g_clock_theme_m5_number_flow,
};
static constexpr size_t s_theme_count = sizeof(s_themes) / sizeof(s_themes[0]);
static size_t s_current_theme_index = 0;

static constexpr gpio_num_t CLOCK_BUTTON_GPIO = GPIO_NUM_0;
static constexpr uint8_t CLOCK_BUTTON_ACTIVE_LEVEL = 0;
static constexpr int32_t CLOCK_DISPLAY_SIZE = 466;

static constexpr uint32_t PWR_BUTTON_POLL_TIMER_MS = 30;
static constexpr uint32_t PWR_BUTTON_DEBOUNCE_MS = 50;
static constexpr uint32_t PWR_BUTTON_SINGLE_CLICK_MAX_MS = 800;
static constexpr uint32_t CLOCK_INPUT_TIMER_MS = 15;
static constexpr uint32_t CLOCK_SETTINGS_SAVE_DELAY_MS = 500;
static constexpr uint32_t PWR_BUTTON_PIN_MASK = IO_EXPANDER_PIN_NUM_4;
static constexpr uint8_t PWR_BUTTON_ACTIVE_LEVEL = 0;

static constexpr uint32_t CLOCK_ROTATION_TIMER_MS = 16;
static constexpr uint32_t CLOCK_THEME_ANIMATION_TIMER_MS = 33;
static constexpr uint32_t CLOCK_ROTATION_PERF_LOG_MS = 1000;
static constexpr float CLOCK_ROTATION_MAX_DT_S = 0.05f;
static constexpr float CLOCK_ROTATION_INVALIDATE_MOVING_EPSILON_DEG = 1.5f;
static constexpr float CLOCK_ROTATION_INVALIDATE_SETTLED_EPSILON_DEG = 2.0f;
static constexpr float CLOCK_ROTATION_START_EPSILON_DEG = 15.0f;
static constexpr float CLOCK_ROTATION_TARGET_EPSILON_DEG = 3.0f;
static constexpr float CLOCK_ROTATION_SETTLED_CORRECTION_EPSILON_DEG = 2.0f;
static constexpr spring_angle_config_t CLOCK_ROTATION_SPRING_CONFIG = {
    .stiffness = 180.0f,
    .damping = 24.0f,
    .snap_error_deg = 2.0f,
    .snap_velocity_dps = 12.0f,
};

static lv_display_t *s_display = NULL;
static clock_settings_t s_settings = {};
static lv_obj_t *s_current_theme_root = NULL;
static lv_obj_t *s_clock_snapshot_image = NULL;
static lv_obj_t *s_settings_menu = NULL;
static lv_draw_buf_t *s_clock_snapshot = NULL;
static lv_timer_t *s_clock_update_timer = NULL;
static lv_timer_t *s_theme_animation_timer = NULL;
static lv_timer_t *s_clock_rotation_timer = NULL;
static lv_timer_t *s_auto_sleep_timer = NULL;
static lv_timer_t *s_input_timer = NULL;
static lv_timer_t *s_settings_save_timer = NULL;
static spring_angle_t s_clock_rotation_spring = {};
static uint32_t s_clock_rotation_last_tick_ms = 0;
static uint32_t s_clock_rotation_last_perf_log_ms = 0;
static uint32_t s_clock_rotation_perf_ticks = 0;
static uint32_t s_clock_rotation_perf_applies = 0;
static uint32_t s_clock_rotation_perf_snapshots = 0;
static uint32_t s_clock_rotation_perf_skip_epsilon = 0;
static uint32_t s_clock_rotation_perf_skip_settled = 0;
static float s_clock_rotation_applied_deg = 0.0f;
static bool s_clock_rotation_applied_valid = false;
static portMUX_TYPE s_clock_rotation_lock = portMUX_INITIALIZER_UNLOCKED;
static float s_clock_rotation_target_deg = 0.0f;
static float s_clock_rotation_submitted_target_deg = 0.0f;
static bool s_clock_rotation_submitted_target_valid = false;
static bool s_clock_rotation_started = false;
static bool s_clock_rotation_target_dirty = false;
static bool s_clock_snapshot_dirty = false;
static bool s_settings_save_pending = false;
static volatile bool s_clock_rendering_enabled = true;
static volatile int s_theme_switch_pending_step = 0;
static volatile bool s_button_single_click_pending = false;
static volatile bool s_pwr_single_click_pending = false;
static volatile bool s_menu_toggle_pending = false;
static uint32_t s_last_activity_ms = 0;
static bool s_touch_action_blocked_until_release = false;
static bool s_screen_sleeping = false;
static esp_io_expander_handle_t s_pwr_io_expander = NULL;
static bool s_pwr_button_ready = false;
static bool s_pwr_button_stable_pressed = false;
static bool s_pwr_button_candidate_pressed = false;
static uint32_t s_pwr_button_candidate_since_ms = 0;
static uint32_t s_pwr_button_press_start_ms = 0;
static uint32_t s_pwr_button_last_poll_ms = 0;

static void keep_touch_layer_foreground(void);
static void close_settings_menu(void);
static void open_settings_menu(void);
static bool note_user_activity(void);
static bool sleep_screen(const char *reason);
static void update_clock_rendering_state(void);
static void update_clock(lv_timer_t *timer);
static void update_theme_animation(lv_timer_t *timer);
static void update_clock_rotation(lv_timer_t *timer);
static void update_pwr_button(lv_timer_t *timer);
static void apply_clock_rotation(float angle_deg);

static bool is_clock_rotation_active(void)
{
    return !spring_angle_is_settled(&s_clock_rotation_spring);
}

static const clock_theme_t *current_theme(void)
{
    return s_themes[s_current_theme_index];
}

static bool has_pending_rotation_target(void)
{
    portENTER_CRITICAL(&s_clock_rotation_lock);
    const bool pending = s_clock_rotation_target_dirty;
    portEXIT_CRITICAL(&s_clock_rotation_lock);
    return pending;
}

static void start_rotation_timer_locked(void)
{
    if (!s_clock_rendering_enabled ||
        s_clock_rotation_timer == NULL ||
        !lv_timer_get_paused(s_clock_rotation_timer)) {
        return;
    }

    s_clock_rotation_last_tick_ms = lv_tick_get();
    lv_timer_resume(s_clock_rotation_timer);
    lv_timer_ready(s_clock_rotation_timer);
}

static void update_clock_rendering_state(void)
{
    const bool enabled = !s_screen_sleeping && s_settings_menu == NULL;
    s_clock_rendering_enabled = enabled;
    imu_demo_set_active(enabled);

    if (!enabled) {
        if (s_clock_update_timer != NULL) {
            lv_timer_pause(s_clock_update_timer);
        }
        if (s_theme_animation_timer != NULL) {
            lv_timer_pause(s_theme_animation_timer);
        }
        if (s_clock_rotation_timer != NULL) {
            lv_timer_pause(s_clock_rotation_timer);
        }
        return;
    }

    s_clock_rotation_last_tick_ms = lv_tick_get();
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

    if (s_clock_rotation_timer != NULL) {
        if (has_pending_rotation_target() || !spring_angle_is_settled(&s_clock_rotation_spring)) {
            start_rotation_timer_locked();
        } else {
            lv_timer_pause(s_clock_rotation_timer);
        }
    }

    update_clock(NULL);
    if (is_clock_rotation_active() || s_clock_snapshot_dirty) {
        apply_clock_rotation(spring_angle_get_current(&s_clock_rotation_spring));
    }
}

static void set_current_theme_overlay_visible(bool visible)
{
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

static void set_current_theme_rotation(float angle_deg)
{
    const clock_theme_t *theme = current_theme();
    if (theme->set_rotation != NULL) {
        theme->set_rotation(angle_deg);
    }
}

static void reset_clock_rotation_perf_counters(void)
{
    s_clock_rotation_perf_ticks = 0;
    s_clock_rotation_perf_applies = 0;
    s_clock_rotation_perf_snapshots = 0;
    s_clock_rotation_perf_skip_epsilon = 0;
    s_clock_rotation_perf_skip_settled = 0;
}

static void apply_clock_rotation(float angle_deg)
{
    if (s_clock_snapshot_image == NULL) {
        return;
    }

    const float invalidate_epsilon = spring_angle_is_settled(&s_clock_rotation_spring)
                                         ? CLOCK_ROTATION_INVALIDATE_SETTLED_EPSILON_DEG
                                         : CLOCK_ROTATION_INVALIDATE_MOVING_EPSILON_DEG;
    if (s_clock_rotation_applied_valid &&
        fabsf(spring_angle_shortest_delta(angle_deg, s_clock_rotation_applied_deg)) <
            invalidate_epsilon) {
        ++s_clock_rotation_perf_skip_epsilon;
        return;
    }

    lv_image_set_rotation(s_clock_snapshot_image, (int32_t)(angle_deg * 10.0f));
    set_current_theme_rotation(angle_deg);
    clock_touch_gesture_set_rotation(angle_deg);
    keep_touch_layer_foreground();
    s_clock_rotation_applied_deg = spring_angle_normalize(angle_deg);
    s_clock_rotation_applied_valid = true;
    ++s_clock_rotation_perf_applies;
}

static void refresh_clock_snapshot(void)
{
    if (s_current_theme_root == NULL || s_clock_snapshot_image == NULL) {
        return;
    }

    lv_obj_clear_flag(s_current_theme_root, LV_OBJ_FLAG_HIDDEN);
    set_current_theme_overlay_visible(false);
    lv_obj_add_flag(s_clock_snapshot_image, LV_OBJ_FLAG_HIDDEN);

    lv_draw_buf_t *snapshot = s_clock_snapshot;
    if (snapshot == NULL) {
        snapshot = lv_snapshot_create_draw_buf(s_current_theme_root, LV_COLOR_FORMAT_RGB565);
        s_clock_snapshot = snapshot;
    }

    lv_result_t snapshot_result = LV_RESULT_INVALID;
    if (snapshot != NULL) {
        snapshot_result =
            lv_snapshot_take_to_draw_buf(s_current_theme_root, LV_COLOR_FORMAT_RGB565, snapshot);
    }

    lv_draw_buf_t *old_snapshot = s_clock_snapshot;
    if (snapshot_result != LV_RESULT_OK) {
        snapshot = lv_snapshot_take(s_current_theme_root, LV_COLOR_FORMAT_RGB565);
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
        s_clock_rotation_applied_valid = false;
        apply_clock_rotation(spring_angle_get_current(&s_clock_rotation_spring));
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
    ++s_clock_rotation_perf_snapshots;
    s_clock_rotation_applied_valid = false;
    apply_clock_rotation(spring_angle_get_current(&s_clock_rotation_spring));
    keep_touch_layer_foreground();
}

static void log_clock_rotation_perf(uint32_t now_ms)
{
    if ((now_ms - s_clock_rotation_last_perf_log_ms) < CLOCK_ROTATION_PERF_LOG_MS) {
        return;
    }
    if (spring_angle_is_settled(&s_clock_rotation_spring)) {
        return;
    }

    s_clock_rotation_last_perf_log_ms = now_ms;
    const uint32_t perf_ticks = s_clock_rotation_perf_ticks;
    const uint32_t perf_applies = s_clock_rotation_perf_applies;
    const uint32_t perf_snapshots = s_clock_rotation_perf_snapshots;
    const uint32_t perf_skip_epsilon = s_clock_rotation_perf_skip_epsilon;
    const uint32_t perf_skip_settled = s_clock_rotation_perf_skip_settled;
    reset_clock_rotation_perf_counters();

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
    uint32_t fps = 0;
    esp_err_t fps_err = s_display != NULL ? esp_lv_adapter_get_fps(s_display, &fps) : ESP_ERR_INVALID_STATE;
    if (fps_err == ESP_OK) {
        ESP_LOGI(TAG,
                 "rotation perf: fps=%" PRIu32
                 ", ticks=%" PRIu32 ", apply=%" PRIu32 ", snapshots=%" PRIu32
                 ", skip_epsilon=%" PRIu32
                 ", skip_settled=%" PRIu32 ", current=%.2f deg, target=%.2f deg",
                 fps,
                 perf_ticks,
                 perf_applies,
                 perf_snapshots,
                 perf_skip_epsilon,
                 perf_skip_settled,
                 spring_angle_get_current(&s_clock_rotation_spring),
                 spring_angle_get_target(&s_clock_rotation_spring));
        return;
    }
#endif

    ESP_LOGI(TAG,
             "rotation perf: ticks=%" PRIu32 ", apply=%" PRIu32
             ", snapshots=%" PRIu32 ", skip_epsilon=%" PRIu32 ", skip_settled=%" PRIu32
             ", current=%.2f deg, target=%.2f deg",
             perf_ticks,
             perf_applies,
             perf_snapshots,
             perf_skip_epsilon,
             perf_skip_settled,
             spring_angle_get_current(&s_clock_rotation_spring),
             spring_angle_get_target(&s_clock_rotation_spring));
}

static void switch_theme(int step);
static void reset_current_theme(void);

static void keep_touch_layer_foreground(void)
{
    if (s_settings_menu != NULL) {
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

    s_theme_switch_pending_step = step;
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

static bool is_touch_action_blocked(void)
{
    return s_touch_action_blocked_until_release;
}

static bool wake_screen_if_sleeping(void)
{
    if (!s_screen_sleeping) {
        return false;
    }

    esp_err_t err = bsp_display_brightness_set(s_settings.brightness_percent);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wake brightness failed: %s", esp_err_to_name(err));
        return false;
    }

    s_screen_sleeping = false;
    s_last_activity_ms = lv_tick_get();
    update_clock_rendering_state();
    return true;
}

static bool sleep_screen(const char *reason)
{
    if (s_screen_sleeping) {
        return false;
    }

    esp_err_t err = bsp_display_brightness_set(0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s sleep brightness failed: %s", reason, esp_err_to_name(err));
        return false;
    }

    s_screen_sleeping = true;
    ESP_LOGI(TAG, "screen %s sleep", reason);
    update_clock_rendering_state();
    return true;
}

static bool note_user_activity(void)
{
    const bool woke = wake_screen_if_sleeping();
    s_last_activity_ms = lv_tick_get();
    if (woke) {
        s_touch_action_blocked_until_release = true;
    }
    return woke;
}

static void toggle_screen_sleep_from_pwr(void)
{
    if (wake_screen_if_sleeping()) {
        return;
    }

    sleep_screen("manual");
}

static void on_menu_activity(void *user_ctx)
{
    (void)user_ctx;
    note_user_activity();
}

static void on_menu_brightness(int brightness_percent, void *user_ctx)
{
    (void)user_ctx;
    s_settings.brightness_percent = brightness_percent;
    if (!s_screen_sleeping) {
        esp_err_t err = bsp_display_brightness_set(s_settings.brightness_percent);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Set brightness failed: %s", esp_err_to_name(err));
        }
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
    s_settings.wifi_enabled = enabled;
    schedule_settings_save();
    ESP_LOGI(TAG, "WiFi setting is %s (connection is not configured yet)", enabled ? "on" : "off");
}

static void on_menu_close(void *user_ctx)
{
    (void)user_ctx;
    close_settings_menu();
}

static void open_settings_menu(void)
{
    if (s_settings_menu != NULL) {
        keep_touch_layer_foreground();
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    clock_settings_menu_config_t menu_config = {
        .settings = &s_settings,
        .brightness_cb = on_menu_brightness,
        .sleep_cb = on_menu_sleep,
        .wifi_cb = on_menu_wifi,
        .close_cb = on_menu_close,
        .activity_cb = on_menu_activity,
        .user_ctx = NULL,
    };
    s_settings_menu = clock_settings_menu_create(screen, &menu_config);
    if (s_settings_menu == NULL) {
        ESP_LOGW(TAG, "Failed to create settings menu");
        return;
    }

    clock_touch_gesture_set_enabled(false);
    keep_touch_layer_foreground();
    update_clock_rendering_state();
}

static void close_settings_menu(void)
{
    if (s_settings_menu == NULL) {
        return;
    }

    flush_settings_save();
    lv_obj_del(s_settings_menu);
    s_settings_menu = NULL;
    clock_touch_gesture_set_enabled(true);
    keep_touch_layer_foreground();
    update_clock_rendering_state();
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
    const bool pwr_single_click = s_pwr_single_click_pending;
    const bool button_single_click = s_button_single_click_pending;
    const bool menu_toggle = s_menu_toggle_pending;
    s_pwr_single_click_pending = false;
    s_button_single_click_pending = false;
    s_menu_toggle_pending = false;

    if (pwr_single_click) {
        toggle_screen_sleep_from_pwr();
        return;
    }

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

static void update_clock_rotation(lv_timer_t *timer)
{
    if (!s_clock_rendering_enabled) {
        lv_timer_pause(timer);
        return;
    }

    ++s_clock_rotation_perf_ticks;

    const uint32_t now_ms = lv_tick_get();
    uint32_t elapsed_ms = now_ms - s_clock_rotation_last_tick_ms;
    s_clock_rotation_last_tick_ms = now_ms;

    if (elapsed_ms == 0) {
        elapsed_ms = CLOCK_ROTATION_TIMER_MS;
    }

    float dt_s = elapsed_ms / 1000.0f;
    if (dt_s > CLOCK_ROTATION_MAX_DT_S) {
        dt_s = CLOCK_ROTATION_MAX_DT_S;
    }

    portENTER_CRITICAL(&s_clock_rotation_lock);
    const bool target_dirty = s_clock_rotation_target_dirty;
    const float target_deg = s_clock_rotation_target_deg;
    s_clock_rotation_target_dirty = false;
    portEXIT_CRITICAL(&s_clock_rotation_lock);

    if (target_dirty) {
        spring_angle_set_target(&s_clock_rotation_spring, target_deg);
    }

    if (!target_dirty && spring_angle_is_settled(&s_clock_rotation_spring)) {
        portENTER_CRITICAL(&s_clock_rotation_lock);
        s_clock_rotation_started = false;
        portEXIT_CRITICAL(&s_clock_rotation_lock);

        ++s_clock_rotation_perf_skip_settled;
        if (s_clock_snapshot_dirty) {
            refresh_clock_snapshot();
        } else {
            apply_clock_rotation(spring_angle_get_current(&s_clock_rotation_spring));
        }
        lv_timer_pause(timer);
        return;
    }

    spring_angle_update(&s_clock_rotation_spring, dt_s);
    if (spring_angle_is_settled(&s_clock_rotation_spring)) {
        portENTER_CRITICAL(&s_clock_rotation_lock);
        s_clock_rotation_started = false;
        portEXIT_CRITICAL(&s_clock_rotation_lock);

        if (s_clock_snapshot_dirty) {
            refresh_clock_snapshot();
        } else {
            apply_clock_rotation(spring_angle_get_current(&s_clock_rotation_spring));
        }
        lv_timer_pause(timer);
        return;
    }

    apply_clock_rotation(spring_angle_get_current(&s_clock_rotation_spring));
    log_clock_rotation_perf(now_ms);
}

static void update_auto_sleep(lv_timer_t *timer)
{
    (void)timer;

    if (s_screen_sleeping || s_settings.sleep_timeout_sec == 0 || s_last_activity_ms == 0) {
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

static void update_pwr_button(lv_timer_t *timer)
{
    (void)timer;

    if (!s_pwr_button_ready || s_pwr_io_expander == NULL) {
        return;
    }

    uint32_t level_mask = 0;
    esp_err_t err = esp_io_expander_get_level(s_pwr_io_expander, PWR_BUTTON_PIN_MASK, &level_mask);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PWR button read failed: %s", esp_err_to_name(err));
        return;
    }

    const bool raw_pressed = ((level_mask & PWR_BUTTON_PIN_MASK) != 0) != (PWR_BUTTON_ACTIVE_LEVEL == 0);
    const uint32_t now_ms = lv_tick_get();
    if (raw_pressed != s_pwr_button_candidate_pressed) {
        s_pwr_button_candidate_pressed = raw_pressed;
        s_pwr_button_candidate_since_ms = now_ms;
        return;
    }

    if (raw_pressed == s_pwr_button_stable_pressed ||
        (uint32_t)(now_ms - s_pwr_button_candidate_since_ms) < PWR_BUTTON_DEBOUNCE_MS) {
        return;
    }

    s_pwr_button_stable_pressed = raw_pressed;
    if (s_pwr_button_stable_pressed) {
        s_pwr_button_press_start_ms = now_ms;
        return;
    }

    const uint32_t press_ms = now_ms - s_pwr_button_press_start_ms;
    if (press_ms <= PWR_BUTTON_SINGLE_CLICK_MAX_MS) {
        s_pwr_single_click_pending = true;
    }
}

static void update_inputs(lv_timer_t *timer)
{
    (void)timer;

    process_pending_input_actions();

    const int theme_switch_step = s_theme_switch_pending_step;
    if (theme_switch_step != 0) {
        s_theme_switch_pending_step = 0;
        switch_theme(theme_switch_step);
    }

    const uint32_t now_ms = lv_tick_get();
    if ((uint32_t)(now_ms - s_pwr_button_last_poll_ms) >= PWR_BUTTON_POLL_TIMER_MS) {
        s_pwr_button_last_poll_ms = now_ms;
        update_pwr_button(NULL);
    }

    if (s_clock_rendering_enabled &&
        has_pending_rotation_target() &&
        s_clock_rotation_timer != NULL) {
        start_rotation_timer_locked();
    }
}

static void on_imu_clock_angle(float angle_deg, void *user_ctx)
{
    (void)user_ctx;

    const float normalized_angle_deg = spring_angle_normalize(angle_deg);

    portENTER_CRITICAL(&s_clock_rotation_lock);
    if (!s_clock_rotation_submitted_target_valid) {
        s_clock_rotation_submitted_target_deg = normalized_angle_deg;
        s_clock_rotation_submitted_target_valid = true;
        portEXIT_CRITICAL(&s_clock_rotation_lock);
        return;
    }

    const float target_delta_deg =
        fabsf(spring_angle_shortest_delta(normalized_angle_deg,
                                          s_clock_rotation_submitted_target_deg));
    const bool rotation_settled = !s_clock_rotation_started &&
                                  spring_angle_is_settled(&s_clock_rotation_spring);
    const float update_epsilon_deg = s_clock_rotation_started
                                         ? CLOCK_ROTATION_TARGET_EPSILON_DEG
                                         : rotation_settled
                                               ? CLOCK_ROTATION_SETTLED_CORRECTION_EPSILON_DEG
                                               : CLOCK_ROTATION_START_EPSILON_DEG;
    if (target_delta_deg < update_epsilon_deg) {
        if (rotation_settled) {
            s_clock_rotation_submitted_target_deg = normalized_angle_deg;
        }
        portEXIT_CRITICAL(&s_clock_rotation_lock);
        return;
    }

    if (s_clock_rotation_started || target_delta_deg >= CLOCK_ROTATION_START_EPSILON_DEG) {
        s_clock_rotation_started = true;
    }
    s_clock_rotation_submitted_target_deg = normalized_angle_deg;
    s_clock_rotation_target_deg = normalized_angle_deg;
    s_clock_rotation_target_dirty = true;
    portEXIT_CRITICAL(&s_clock_rotation_lock);
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

    if (s_clock_snapshot_dirty && !is_clock_rotation_active()) {
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
    if (is_clock_rotation_active()) {
        return;
    }

    if (theme->tick(lv_tick_get())) {
        if (theme->tick_updates_overlay_only == NULL || !theme->tick_updates_overlay_only()) {
            s_clock_snapshot_dirty = true;
            if (!is_clock_rotation_active()) {
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

    lv_obj_t *screen = lv_screen_active();
    set_current_theme_overlay_visible(false);
    destroy_current_theme_overlay();
    lv_obj_del(s_current_theme_root);

    const int normalized_step = step % (int)s_theme_count;
    s_current_theme_index =
        (s_current_theme_index + s_theme_count + normalized_step) % s_theme_count;
    s_current_theme_root = s_themes[s_current_theme_index]->build(screen);
    ESP_LOGI(TAG, "switch theme -> %s", s_themes[s_current_theme_index]->name);

    s_clock_rotation_applied_valid = false;
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

    s_current_theme_root = s_themes[s_current_theme_index]->build(screen);
    reset_current_theme();

    s_clock_snapshot_image = lv_image_create(screen);
    lv_obj_center(s_clock_snapshot_image);
    s_clock_snapshot =
        lv_draw_buf_create(CLOCK_DISPLAY_SIZE,
                           CLOCK_DISPLAY_SIZE,
                           LV_COLOR_FORMAT_RGB565,
                           LV_STRIDE_AUTO);
    if (s_clock_snapshot == NULL) {
        ESP_LOGW(TAG, "Failed to allocate reusable clock snapshot buffer");
    }

    clock_touch_gesture_create(screen,
                               on_touch_theme_switch,
                               NULL,
                               on_touch_long_press,
                               NULL,
                               on_touch_activity,
                               NULL);

    spring_angle_init(&s_clock_rotation_spring, &CLOCK_ROTATION_SPRING_CONFIG, 0.0f);
    s_clock_rotation_last_tick_ms = lv_tick_get();
    s_clock_rotation_last_perf_log_ms = s_clock_rotation_last_tick_ms;
    s_clock_rotation_applied_valid = false;
    portENTER_CRITICAL(&s_clock_rotation_lock);
    s_clock_rotation_target_deg = spring_angle_get_target(&s_clock_rotation_spring);
    s_clock_rotation_submitted_target_deg = s_clock_rotation_target_deg;
    s_clock_rotation_submitted_target_valid = false;
    s_clock_rotation_started = false;
    s_clock_rotation_target_dirty = false;
    portEXIT_CRITICAL(&s_clock_rotation_lock);

    s_last_activity_ms = lv_tick_get();
    s_clock_snapshot_dirty = true;
    s_clock_update_timer = lv_timer_create(update_clock, 1000, NULL);
    s_theme_animation_timer =
        lv_timer_create(update_theme_animation, CLOCK_THEME_ANIMATION_TIMER_MS, NULL);
    s_clock_rotation_timer =
        lv_timer_create(update_clock_rotation, CLOCK_ROTATION_TIMER_MS, NULL);
    if (s_clock_rotation_timer != NULL) {
        lv_timer_pause(s_clock_rotation_timer);
    }
    s_auto_sleep_timer = lv_timer_create(update_auto_sleep, 1000, NULL);
    s_input_timer = lv_timer_create(update_inputs, CLOCK_INPUT_TIMER_MS, NULL);

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

static void pwr_button_init(void)
{
    s_pwr_io_expander = bsp_io_expander_init();
    if (s_pwr_io_expander == NULL) {
        ESP_LOGW(TAG, "PWR button IO expander init failed");
        return;
    }

    esp_err_t err = esp_io_expander_set_dir(s_pwr_io_expander, PWR_BUTTON_PIN_MASK, IO_EXPANDER_INPUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PWR button input config failed: %s", esp_err_to_name(err));
        return;
    }

    uint32_t level_mask = 0;
    err = esp_io_expander_get_level(s_pwr_io_expander, PWR_BUTTON_PIN_MASK, &level_mask);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PWR button initial read failed: %s", esp_err_to_name(err));
        return;
    }

    const bool pressed = ((level_mask & PWR_BUTTON_PIN_MASK) != 0) != (PWR_BUTTON_ACTIVE_LEVEL == 0);
    const uint32_t now_ms = lv_tick_get();
    s_pwr_button_stable_pressed = pressed;
    s_pwr_button_candidate_pressed = pressed;
    s_pwr_button_candidate_since_ms = now_ms;
    s_pwr_button_press_start_ms = pressed ? now_ms : 0;
    s_pwr_button_last_poll_ms = now_ms;
    s_pwr_button_ready = true;
    ESP_LOGI(TAG, "PWR button polling on EXIO4");
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Start simple clock demo");

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

    rtc_time_sync_from_external_rtc();

    s_display = bsp_display_start();
    ESP_ERROR_CHECK(s_display ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(bsp_display_brightness_set(s_settings.brightness_percent));
    ESP_LOGI(TAG, "touch indev: %p", bsp_display_get_input_dev());

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
    ESP_ERROR_CHECK(esp_lv_adapter_fps_stats_enable(s_display, true));
#endif

    ESP_ERROR_CHECK(bsp_display_lock(UINT32_MAX));
    create_clock_ui();
    bsp_display_unlock();

    clock_button_init();
    pwr_button_init();
    imu_demo_start(on_imu_clock_angle, NULL);
}
