#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool initialized;
    bool battery_present;
    bool charging;
    bool discharging;
    bool full;
    bool vbus_present;
    uint16_t battery_mv;
    uint16_t vbus_mv;
    uint16_t system_mv;
    uint8_t battery_percent;
    bool battery_percent_estimated;
} power_status_t;

esp_err_t power_manager_init(void);
void power_manager_update(void);
void power_manager_get_status(power_status_t *status);
esp_err_t power_manager_shutdown(void);

#ifdef __cplusplus
}
#endif
