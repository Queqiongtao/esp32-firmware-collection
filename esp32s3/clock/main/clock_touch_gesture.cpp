#include "clock_touch_gesture.h"

#include <inttypes.h>
#include <math.h>

#include "esp_log.h"
#include "spring_angle.h"

static const char *TAG = "ClockTouch";

static constexpr int32_t CLOCK_TOUCH_LAYER_SIZE_PX = 466;
static constexpr uint32_t CLOCK_TOUCH_SWITCH_COOLDOWN_MS = 350;
static constexpr uint32_t CLOCK_TOUCH_LONG_PRESS_MS = 2000;
static constexpr int32_t CLOCK_TOUCH_LONG_PRESS_MOVE_TOLERANCE_PX = 16;
static constexpr int32_t CLOCK_TOUCH_SWIPE_MIN_DELTA_PX = 100;
static constexpr float CLOCK_TOUCH_SWIPE_AXIS_RATIO = 2.0f;
static constexpr float DEG_TO_RAD = 0.017453292519943295f;

static lv_obj_t *s_touch_layer = NULL;
static clock_touch_gesture_cb_t s_touch_cb = NULL;
static void *s_touch_user_ctx = NULL;
static clock_touch_long_press_cb_t s_long_press_cb = NULL;
static void *s_long_press_user_ctx = NULL;
static clock_touch_activity_cb_t s_activity_cb = NULL;
static void *s_activity_user_ctx = NULL;
static uint32_t s_last_theme_switch_ms = 0;
static lv_point_t s_press_point = {};
static bool s_press_valid = false;
static bool s_press_moved = false;
static bool s_long_press_fired = false;
static bool s_touch_enabled = true;
static float s_rotation_deg = 0.0f;

static bool get_touch_point(lv_event_t *event, lv_point_t *point)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        indev = lv_event_get_indev(event);
    }
    if (indev == NULL) {
        return false;
    }

    lv_indev_get_point(indev, point);
    return true;
}

static void rotate_to_clock_local(int32_t dx, int32_t dy, float *local_dx, float *local_dy)
{
    const float radians = -s_rotation_deg * DEG_TO_RAD;
    const float c = cosf(radians);
    const float s = sinf(radians);

    *local_dx = (float)dx * c - (float)dy * s;
    *local_dy = (float)dx * s + (float)dy * c;
}

static bool is_swipe_allowed(uint32_t now_ms)
{
    return (uint32_t)(now_ms - s_last_theme_switch_ms) >= CLOCK_TOUCH_SWITCH_COOLDOWN_MS;
}

static void notify_activity(lv_event_code_t code)
{
    if (s_activity_cb != NULL) {
        s_activity_cb(code, s_activity_user_ctx);
    }
}

static bool point_moved_beyond_long_press_tolerance(const lv_point_t *point)
{
    const int32_t dx = point->x - s_press_point.x;
    const int32_t dy = point->y - s_press_point.y;
    const int32_t tolerance = CLOCK_TOUCH_LONG_PRESS_MOVE_TOLERANCE_PX;
    return (dx * dx + dy * dy) >= (tolerance * tolerance);
}

static void on_clock_touch_event(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_PRESSED) {
        notify_activity(code);
        if (get_touch_point(event, &s_press_point)) {
            s_press_valid = true;
            s_press_moved = false;
            s_long_press_fired = false;
            ESP_LOGD(TAG, "touch pressed: x=%" PRId32 ", y=%" PRId32,
                     s_press_point.x, s_press_point.y);
        }
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        lv_point_t point = {};
        if (s_press_valid && !s_press_moved && get_touch_point(event, &point) &&
            point_moved_beyond_long_press_tolerance(&point)) {
            s_press_moved = true;
        }
        return;
    }

    if (code == LV_EVENT_LONG_PRESSED) {
        notify_activity(code);
        lv_point_t point = {};
        const bool moved =
            s_press_moved ||
            (s_press_valid && get_touch_point(event, &point) &&
             point_moved_beyond_long_press_tolerance(&point));
        if (!s_press_valid || moved) {
            s_press_moved = moved;
            return;
        }

        s_long_press_fired = true;
        s_press_valid = false;
        if (s_touch_enabled && s_long_press_cb != NULL) {
            s_long_press_cb(s_long_press_user_ctx);
        }
        return;
    }

    if (code == LV_EVENT_PRESS_LOST) {
        s_press_valid = false;
        s_press_moved = false;
        s_long_press_fired = false;
        notify_activity(code);
        return;
    }

    if (code != LV_EVENT_RELEASED) {
        return;
    }

    if (!s_touch_enabled || s_long_press_fired) {
        s_press_valid = false;
        s_press_moved = false;
        s_long_press_fired = false;
        notify_activity(code);
        return;
    }

    lv_point_t release_point = {};
    if (!get_touch_point(event, &release_point)) {
        s_press_valid = false;
        s_press_moved = false;
        notify_activity(code);
        return;
    }

    if (!s_press_valid) {
        s_press_moved = false;
        notify_activity(code);
        return;
    }
    s_press_valid = false;
    s_press_moved = false;

    const uint32_t now_ms = lv_tick_get();

    const int32_t dx = release_point.x - s_press_point.x;
    const int32_t dy = release_point.y - s_press_point.y;
    float local_dx = 0.0f;
    float local_dy = 0.0f;
    rotate_to_clock_local(dx, dy, &local_dx, &local_dy);

    const float abs_local_dx = fabsf(local_dx);
    const float abs_local_dy = fabsf(local_dy);
    if (abs_local_dx < CLOCK_TOUCH_SWIPE_MIN_DELTA_PX ||
        abs_local_dx <= abs_local_dy * CLOCK_TOUCH_SWIPE_AXIS_RATIO) {
        ESP_LOGD(TAG,
                 "touch ignored: raw=(%" PRId32 ",%" PRId32 "), local=(%.1f,%.1f), rot=%.1f",
                 dx,
                 dy,
                 local_dx,
                 local_dy,
                 s_rotation_deg);
        notify_activity(code);
        return;
    }

    if (!is_swipe_allowed(now_ms)) {
        notify_activity(code);
        return;
    }

    s_last_theme_switch_ms = now_ms;
    const int step = local_dx < 0.0f ? 1 : -1;
    ESP_LOGI(TAG,
             "touch swipe: step=%d, raw=(%" PRId32 ",%" PRId32 "), local=(%.1f,%.1f), rot=%.1f",
             step,
             dx,
             dy,
             local_dx,
             local_dy,
             s_rotation_deg);

    if (s_touch_cb != NULL) {
        s_touch_cb(step, s_touch_user_ctx);
    }
    notify_activity(code);
}

lv_obj_t *clock_touch_gesture_create(lv_obj_t *parent,
                                     lv_indev_t *indev,
                                     clock_touch_gesture_cb_t cb,
                                     void *user_ctx,
                                     clock_touch_long_press_cb_t long_press_cb,
                                     void *long_press_user_ctx,
                                     clock_touch_activity_cb_t activity_cb,
                                     void *activity_user_ctx)
{
    if (indev != NULL) {
        lv_indev_set_long_press_time(indev, CLOCK_TOUCH_LONG_PRESS_MS);
    }

    s_touch_cb = cb;
    s_touch_user_ctx = user_ctx;
    s_long_press_cb = long_press_cb;
    s_long_press_user_ctx = long_press_user_ctx;
    s_activity_cb = activity_cb;
    s_activity_user_ctx = activity_user_ctx;
    s_last_theme_switch_ms = 0;
    s_press_point = {};
    s_press_valid = false;
    s_press_moved = false;
    s_long_press_fired = false;
    s_touch_enabled = true;
    s_rotation_deg = 0.0f;

    s_touch_layer = lv_obj_create(parent);
    lv_obj_remove_style_all(s_touch_layer);
    lv_obj_clear_flag(s_touch_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_touch_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_touch_layer, CLOCK_TOUCH_LAYER_SIZE_PX, CLOCK_TOUCH_LAYER_SIZE_PX);
    lv_obj_center(s_touch_layer);
    lv_obj_add_event_cb(s_touch_layer, on_clock_touch_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_touch_layer, on_clock_touch_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_touch_layer, on_clock_touch_event, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(s_touch_layer, on_clock_touch_event, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(s_touch_layer, on_clock_touch_event, LV_EVENT_RELEASED, NULL);

    return s_touch_layer;
}

void clock_touch_gesture_set_enabled(bool enabled)
{
    s_touch_enabled = enabled;
}

void clock_touch_gesture_set_rotation(float angle_deg)
{
    s_rotation_deg = spring_angle_normalize(angle_deg);
}

void clock_touch_gesture_move_foreground(void)
{
    if (s_touch_layer != NULL) {
        lv_obj_move_foreground(s_touch_layer);
    }
}
