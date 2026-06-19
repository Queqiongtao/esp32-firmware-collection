#include "spring_angle.h"

#include <math.h>
#include <string.h>

static constexpr spring_angle_config_t DEFAULT_CONFIG = {
    .stiffness = 35.0f,
    .damping = 9.0f,
    .snap_error_deg = 0.5f,
    .snap_velocity_dps = 5.0f,
};

float spring_angle_normalize(float angle_deg)
{
    while (angle_deg >= 360.0f) {
        angle_deg -= 360.0f;
    }
    while (angle_deg < 0.0f) {
        angle_deg += 360.0f;
    }
    return angle_deg;
}

float spring_angle_shortest_delta(float target_deg, float current_deg)
{
    float delta = spring_angle_normalize(target_deg) - spring_angle_normalize(current_deg);
    while (delta > 180.0f) {
        delta -= 360.0f;
    }
    while (delta < -180.0f) {
        delta += 360.0f;
    }
    return delta;
}

void spring_angle_init(spring_angle_t *spring,
                       const spring_angle_config_t *config,
                       float initial_deg)
{
    if (spring == NULL) {
        return;
    }

    memset(spring, 0, sizeof(*spring));
    spring->config = config != NULL ? *config : DEFAULT_CONFIG;
    spring->current_deg = spring_angle_normalize(initial_deg);
    spring->target_deg = spring->current_deg;
}

void spring_angle_set_target(spring_angle_t *spring, float target_deg)
{
    if (spring == NULL) {
        return;
    }

    spring->target_deg = spring_angle_normalize(target_deg);
}

void spring_angle_update(spring_angle_t *spring, float dt_s)
{
    if (spring == NULL || dt_s <= 0.0f) {
        return;
    }

    const float error = spring_angle_shortest_delta(spring->target_deg, spring->current_deg);
    const float acceleration = spring->config.stiffness * error -
                               spring->config.damping * spring->velocity_dps;

    spring->velocity_dps += acceleration * dt_s;
    spring->current_deg = spring_angle_normalize(spring->current_deg + spring->velocity_dps * dt_s);

    if (fabsf(error) <= spring->config.snap_error_deg &&
        fabsf(spring->velocity_dps) <= spring->config.snap_velocity_dps) {
        spring->current_deg = spring->target_deg;
        spring->velocity_dps = 0.0f;
    }
}

float spring_angle_get_current(const spring_angle_t *spring)
{
    return spring != NULL ? spring->current_deg : 0.0f;
}

float spring_angle_get_target(const spring_angle_t *spring)
{
    return spring != NULL ? spring->target_deg : 0.0f;
}

bool spring_angle_is_settled(const spring_angle_t *spring)
{
    if (spring == NULL) {
        return true;
    }

    return fabsf(spring_angle_shortest_delta(spring->target_deg, spring->current_deg)) <=
               spring->config.snap_error_deg &&
           fabsf(spring->velocity_dps) <= spring->config.snap_velocity_dps;
}
