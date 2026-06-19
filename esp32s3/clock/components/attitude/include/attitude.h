#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float kp;
    float ki;
} attitude_config_t;

typedef struct {
    float q0;
    float q1;
    float q2;
    float q3;
    float integral_error[3];
    float kp;
    float ki;
} attitude_t;

typedef struct {
    float roll_deg;
    float pitch_deg;
    float yaw_deg;
    bool yaw_is_absolute;
} attitude_angles_t;

void attitude_init(attitude_t *attitude, const attitude_config_t *config);
void attitude_update(attitude_t *attitude,
                     float accel_g[3],
                     float gyro_dps[3],
                     float dt_s);
void attitude_get_angles(const attitude_t *attitude, attitude_angles_t *angles);

#ifdef __cplusplus
}
#endif
