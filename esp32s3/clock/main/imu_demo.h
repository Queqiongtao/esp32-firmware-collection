#pragma once

#include "esp_err.h"

typedef void (*imu_demo_angle_cb_t)(float angle_deg, void *user_ctx);

esp_err_t imu_demo_start(imu_demo_angle_cb_t angle_cb,
                         void *angle_user_ctx);
