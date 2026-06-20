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
#include "clock_theme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "imu_demo.h"
#include "iot_button.h"
#include "lvgl.h"
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

static constexpr uint32_t CLOCK_ROTATION_TIMER_MS = 16;
static constexpr uint32_t CLOCK_THEME_ANIMATION_TIMER_MS = 33;
static constexpr uint32_t CLOCK_ROTATION_PERF_LOG_MS = 1000;
static constexpr float CLOCK_ROTATION_MAX_DT_S = 0.05f;
static constexpr float CLOCK_ROTATION_INVALIDATE_MOVING_EPSILON_DEG = 1.5f;
static constexpr float CLOCK_ROTATION_INVALIDATE_SETTLED_EPSILON_DEG = 2.0f;
static constexpr float CLOCK_ROTATION_START_EPSILON_DEG = 15.0f;
static constexpr float CLOCK_ROTATION_TARGET_EPSILON_DEG = 3.0f;
static constexpr spring_angle_config_t CLOCK_ROTATION_SPRING_CONFIG = {
    .stiffness = 180.0f,
    .damping = 24.0f,
    .snap_error_deg = 2.0f,
    .snap_velocity_dps = 12.0f,
};

static lv_display_t *s_display = NULL;
static lv_obj_t *s_current_theme_root = NULL;
static lv_obj_t *s_clock_snapshot_image = NULL;
static lv_draw_buf_t *s_clock_snapshot = NULL;
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
static volatile bool s_theme_switch_pending = false;

static bool is_clock_rotation_active(void)
{
    return !spring_angle_is_settled(&s_clock_rotation_spring);
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
    lv_obj_add_flag(s_clock_snapshot_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_current_theme_root);

    lv_draw_buf_t *snapshot = lv_snapshot_take(s_current_theme_root, LV_COLOR_FORMAT_RGB565);
    lv_obj_add_flag(s_current_theme_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_clock_snapshot_image, LV_OBJ_FLAG_HIDDEN);

    if (snapshot == NULL) {
        ESP_LOGW(TAG, "Failed to refresh clock snapshot");
        return;
    }

    lv_draw_buf_t *old_snapshot = s_clock_snapshot;
    s_clock_snapshot = snapshot;
    lv_image_set_src(s_clock_snapshot_image, s_clock_snapshot);

    const int32_t image_width = lv_image_get_src_width(s_clock_snapshot_image);
    const int32_t image_height = lv_image_get_src_height(s_clock_snapshot_image);
    lv_image_set_pivot(s_clock_snapshot_image, image_width / 2, image_height / 2);
    lv_obj_center(s_clock_snapshot_image);

    if (old_snapshot != NULL) {
        lv_draw_buf_destroy(old_snapshot);
    }

    ++s_clock_rotation_perf_snapshots;
    s_clock_rotation_applied_valid = false;
    apply_clock_rotation(spring_angle_get_current(&s_clock_rotation_spring));
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

static void switch_to_next_theme(void);
static void reset_current_theme(void);

static void reset_current_theme(void)
{
    const clock_theme_t *theme = s_themes[s_current_theme_index];
    if (theme->reset != NULL) {
        theme->reset();
    }
}

static void update_clock_rotation(lv_timer_t *timer)
{
    (void)timer;

    if (s_theme_switch_pending) {
        s_theme_switch_pending = false;
        switch_to_next_theme();
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
    } else if (spring_angle_is_settled(&s_clock_rotation_spring)) {
        portENTER_CRITICAL(&s_clock_rotation_lock);
        s_clock_rotation_started = false;
        s_clock_rotation_submitted_target_deg =
            spring_angle_get_target(&s_clock_rotation_spring);
        s_clock_rotation_submitted_target_valid = true;
        portEXIT_CRITICAL(&s_clock_rotation_lock);

        ++s_clock_rotation_perf_skip_settled;
        if ((uint32_t)(now_ms - s_clock_rotation_last_perf_log_ms) >= CLOCK_ROTATION_PERF_LOG_MS) {
            s_clock_rotation_last_perf_log_ms = now_ms;
            reset_clock_rotation_perf_counters();
        }
        return;
    }

    spring_angle_update(&s_clock_rotation_spring, dt_s);
    apply_clock_rotation(spring_angle_get_current(&s_clock_rotation_spring));
    log_clock_rotation_perf(now_ms);
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
    const float update_epsilon_deg = s_clock_rotation_started
                                         ? CLOCK_ROTATION_TARGET_EPSILON_DEG
                                         : CLOCK_ROTATION_START_EPSILON_DEG;
    if (target_delta_deg < update_epsilon_deg) {
        portEXIT_CRITICAL(&s_clock_rotation_lock);
        return;
    }

    s_clock_rotation_started = true;
    s_clock_rotation_submitted_target_deg = normalized_angle_deg;
    s_clock_rotation_target_deg = normalized_angle_deg;
    s_clock_rotation_target_dirty = true;
    portEXIT_CRITICAL(&s_clock_rotation_lock);
}

static void update_clock(lv_timer_t *timer)
{
    (void)timer;

    time_t now = 0;
    struct tm timeinfo = {};

    time(&now);
    localtime_r(&now, &timeinfo);

    s_themes[s_current_theme_index]->update(&timeinfo);
    refresh_clock_snapshot();
}

static void update_theme_animation(lv_timer_t *timer)
{
    (void)timer;

    const clock_theme_t *theme = s_themes[s_current_theme_index];
    if (theme->tick == NULL) {
        return;
    }
    if (is_clock_rotation_active()) {
        return;
    }

    if (theme->tick(lv_tick_get())) {
        refresh_clock_snapshot();
    }
}

static void switch_to_next_theme(void)
{
    if (s_theme_count <= 1 || s_current_theme_root == NULL) {
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_del(s_current_theme_root);

    s_current_theme_index = (s_current_theme_index + 1) % s_theme_count;
    s_current_theme_root = s_themes[s_current_theme_index]->build(screen);
    ESP_LOGI(TAG, "switch theme -> %s", s_themes[s_current_theme_index]->name);

    s_clock_rotation_applied_valid = false;
    reset_current_theme();
    update_clock(NULL);
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

    update_clock(NULL);
    lv_timer_create(update_clock, 1000, NULL);
    lv_timer_create(update_theme_animation, CLOCK_THEME_ANIMATION_TIMER_MS, NULL);
    lv_timer_create(update_clock_rotation, CLOCK_ROTATION_TIMER_MS, NULL);
}

static void on_theme_button(void *button_handle, void *user_ctx)
{
    (void)button_handle;
    (void)user_ctx;
    s_theme_switch_pending = true;
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
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Start simple clock demo");

    setenv("TZ", "CST-8", 1);
    tzset();

    rtc_time_sync_from_external_rtc();

    s_display = bsp_display_start();
    ESP_ERROR_CHECK(s_display ? ESP_OK : ESP_FAIL);

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
    ESP_ERROR_CHECK(esp_lv_adapter_fps_stats_enable(s_display, true));
#endif

    ESP_ERROR_CHECK(bsp_display_lock(UINT32_MAX));
    create_clock_ui();
    bsp_display_unlock();

    clock_button_init();
    imu_demo_start(on_imu_clock_angle, NULL);
}
