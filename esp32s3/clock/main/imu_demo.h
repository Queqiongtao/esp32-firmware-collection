#pragma once

#include "esp_err.h"

typedef void (*imu_demo_angle_cb_t)(float angle_deg, void *user_ctx);
typedef enum {
    IMU_DEMO_GESTURE_THEME_NEXT = 0,
} imu_demo_gesture_t;

typedef void (*imu_demo_gesture_cb_t)(imu_demo_gesture_t gesture, void *user_ctx);

esp_err_t imu_demo_start(imu_demo_angle_cb_t angle_cb,
                         void *angle_user_ctx,
                         imu_demo_gesture_cb_t gesture_cb,
                         void *gesture_user_ctx);
