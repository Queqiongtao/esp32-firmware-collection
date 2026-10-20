#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*clock_touch_gesture_cb_t)(int step, void *user_ctx);
typedef void (*clock_touch_long_press_cb_t)(void *user_ctx);
typedef void (*clock_touch_activity_cb_t)(lv_event_code_t code, void *user_ctx);

lv_obj_t *clock_touch_gesture_create(lv_obj_t *parent,
                                     lv_indev_t *indev,
                                     clock_touch_gesture_cb_t cb,
                                     void *user_ctx,
                                     clock_touch_long_press_cb_t long_press_cb,
                                     void *long_press_user_ctx,
                                     clock_touch_activity_cb_t activity_cb,
                                     void *activity_user_ctx);
void clock_touch_gesture_set_enabled(bool enabled);
void clock_touch_gesture_set_rotation(float angle_deg);
void clock_touch_gesture_move_foreground(void);

#ifdef __cplusplus
}
#endif
