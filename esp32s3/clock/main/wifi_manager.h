#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MANAGER_OFF = 0,
    WIFI_MANAGER_IDLE,
    WIFI_MANAGER_CONNECTING,
    WIFI_MANAGER_CONNECTED,
    WIFI_MANAGER_FAILED,
} wifi_manager_state_t;

typedef struct {
    wifi_manager_state_t state;
    bool enabled;
    bool has_credentials;
    char ssid[33];
    char ip[16];
} wifi_manager_status_t;

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_set_enabled(bool enabled);
bool wifi_manager_is_enabled(void);
bool wifi_manager_has_credentials(void);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
esp_err_t wifi_manager_forget(void);
void wifi_manager_get_status(wifi_manager_status_t *status);

#ifdef __cplusplus
}
#endif
