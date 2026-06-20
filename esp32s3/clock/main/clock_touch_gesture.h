#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*clock_touch_gesture_cb_t)(int step, void *user_ctx);

lv_obj_t *clock_touch_gesture_create(lv_obj_t *parent,
                                     clock_touch_gesture_cb_t cb,
                                     void *user_ctx);
void clock_touch_gesture_set_rotation(float angle_deg);
void clock_touch_gesture_move_foreground(void);

#ifdef __cplusplus
}
#endif
