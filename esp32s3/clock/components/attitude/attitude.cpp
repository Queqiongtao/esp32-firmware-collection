#include "attitude.h"

#include <math.h>
#include <string.h>

static constexpr float DEFAULT_KP = 2.0f;
static constexpr float DEFAULT_KI = 0.0f;
static constexpr float DEG_TO_RAD = 0.017453292519943295f;
static constexpr float RAD_TO_DEG = 57.29577951308232f;

static float inv_sqrt(float value)
{
    return 1.0f / sqrtf(value);
}

static void normalize_quaternion(attitude_t *attitude)
{
    const float norm = inv_sqrt(attitude->q0 * attitude->q0 +
                                attitude->q1 * attitude->q1 +
                                attitude->q2 * attitude->q2 +
                                attitude->q3 * attitude->q3);
    attitude->q0 *= norm;
    attitude->q1 *= norm;
    attitude->q2 *= norm;
    attitude->q3 *= norm;
}

void attitude_init(attitude_t *attitude, const attitude_config_t *config)
{
    if (attitude == NULL) {
        return;
    }

    memset(attitude, 0, sizeof(*attitude));
    attitude->q0 = 1.0f;
    attitude->kp = config != NULL ? config->kp : DEFAULT_KP;
    attitude->ki = config != NULL ? config->ki : DEFAULT_KI;
}

void attitude_update(attitude_t *attitude,
                     float accel_g[3],
                     float gyro_dps[3],
                     float dt_s)
{
    if (attitude == NULL || accel_g == NULL || gyro_dps == NULL || dt_s <= 0.0f) {
        return;
    }

    float gx = gyro_dps[0] * DEG_TO_RAD;
    float gy = gyro_dps[1] * DEG_TO_RAD;
    float gz = gyro_dps[2] * DEG_TO_RAD;

    const float ax = accel_g[0];
    const float ay = accel_g[1];
    const float az = accel_g[2];
    const float accel_norm_sq = ax * ax + ay * ay + az * az;

    if (accel_norm_sq > 0.000001f) {
        const float recip_norm = inv_sqrt(accel_norm_sq);
        const float axn = ax * recip_norm;
        const float ayn = ay * recip_norm;
        const float azn = az * recip_norm;

        const float vx = 2.0f * (attitude->q1 * attitude->q3 - attitude->q0 * attitude->q2);
        const float vy = 2.0f * (attitude->q0 * attitude->q1 + attitude->q2 * attitude->q3);
        const float vz = attitude->q0 * attitude->q0 - attitude->q1 * attitude->q1 -
                         attitude->q2 * attitude->q2 + attitude->q3 * attitude->q3;

        const float ex = ayn * vz - azn * vy;
        const float ey = azn * vx - axn * vz;
        const float ez = axn * vy - ayn * vx;

        if (attitude->ki > 0.0f) {
            attitude->integral_error[0] += attitude->ki * ex * dt_s;
            attitude->integral_error[1] += attitude->ki * ey * dt_s;
            attitude->integral_error[2] += attitude->ki * ez * dt_s;
            gx += attitude->integral_error[0];
            gy += attitude->integral_error[1];
            gz += attitude->integral_error[2];
        } else {
            attitude->integral_error[0] = 0.0f;
            attitude->integral_error[1] = 0.0f;
            attitude->integral_error[2] = 0.0f;
        }

        gx += attitude->kp * ex;
        gy += attitude->kp * ey;
        gz += attitude->kp * ez;
    }

    gx *= 0.5f * dt_s;
    gy *= 0.5f * dt_s;
    gz *= 0.5f * dt_s;

    const float qa = attitude->q0;
    const float qb = attitude->q1;
    const float qc = attitude->q2;

    attitude->q0 += -qb * gx - qc * gy - attitude->q3 * gz;
    attitude->q1 += qa * gx + qc * gz - attitude->q3 * gy;
    attitude->q2 += qa * gy - qb * gz + attitude->q3 * gx;
    attitude->q3 += qa * gz + qb * gy - qc * gx;

    normalize_quaternion(attitude);
}

void attitude_get_angles(const attitude_t *attitude, attitude_angles_t *angles)
{
    if (attitude == NULL || angles == NULL) {
        return;
    }

    const float q0 = attitude->q0;
    const float q1 = attitude->q1;
    const float q2 = attitude->q2;
    const float q3 = attitude->q3;

    angles->roll_deg = atan2f(2.0f * (q0 * q1 + q2 * q3),
                              1.0f - 2.0f * (q1 * q1 + q2 * q2)) * RAD_TO_DEG;
    angles->pitch_deg = asinf(2.0f * (q0 * q2 - q3 * q1)) * RAD_TO_DEG;
    angles->yaw_deg = atan2f(2.0f * (q0 * q3 + q1 * q2),
                             1.0f - 2.0f * (q2 * q2 + q3 * q3)) * RAD_TO_DEG;
    angles->yaw_is_absolute = false;
}
