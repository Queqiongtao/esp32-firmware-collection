#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float stiffness;
    float damping;
    float snap_error_deg;
    float snap_velocity_dps;
} spring_angle_config_t;

typedef struct {
    float current_deg;
    float target_deg;
    float velocity_dps;
    spring_angle_config_t config;
} spring_angle_t;

void spring_angle_init(spring_angle_t *spring,
                       const spring_angle_config_t *config,
                       float initial_deg);
void spring_angle_set_target(spring_angle_t *spring, float target_deg);
void spring_angle_update(spring_angle_t *spring, float dt_s);
float spring_angle_get_current(const spring_angle_t *spring);
float spring_angle_get_target(const spring_angle_t *spring);
bool spring_angle_is_settled(const spring_angle_t *spring);
float spring_angle_normalize(float angle_deg);
float spring_angle_shortest_delta(float target_deg, float current_deg);

#ifdef __cplusplus
}
#endif
