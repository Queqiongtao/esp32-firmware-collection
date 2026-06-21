#include "clock_settings.h"

#include "nvs.h"

static constexpr const char *CLOCK_SETTINGS_NAMESPACE = "clock_settings";
static constexpr const char *CLOCK_SETTINGS_KEY_BRIGHTNESS = "brightness";
static constexpr const char *CLOCK_SETTINGS_KEY_SLEEP = "sleep_sec";
static constexpr const char *CLOCK_SETTINGS_KEY_WIFI = "wifi";

static int clamp_brightness(int brightness_percent)
{
    if (brightness_percent < 1) {
        return 1;
    }
    if (brightness_percent > 100) {
        return 100;
    }
    return brightness_percent;
}

void clock_settings_defaults(clock_settings_t *settings)
{
    if (settings == nullptr) {
        return;
    }

    settings->brightness_percent = 70;
    settings->sleep_timeout_sec = 5 * 60;
    settings->wifi_enabled = false;
}

esp_err_t clock_settings_load(clock_settings_t *settings)
{
    if (settings == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    clock_settings_defaults(settings);

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(CLOCK_SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t brightness = 0;
    err = nvs_get_u8(handle, CLOCK_SETTINGS_KEY_BRIGHTNESS, &brightness);
    if (err == ESP_OK) {
        settings->brightness_percent = clamp_brightness(brightness);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return err;
    }

    uint32_t sleep_timeout_sec = 0;
    err = nvs_get_u32(handle, CLOCK_SETTINGS_KEY_SLEEP, &sleep_timeout_sec);
    if (err == ESP_OK) {
        settings->sleep_timeout_sec = sleep_timeout_sec;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return err;
    }

    uint8_t wifi_enabled = 0;
    err = nvs_get_u8(handle, CLOCK_SETTINGS_KEY_WIFI, &wifi_enabled);
    if (err == ESP_OK) {
        settings->wifi_enabled = wifi_enabled != 0;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return err;
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t clock_settings_save(const clock_settings_t *settings)
{
    if (settings == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(CLOCK_SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle,
                     CLOCK_SETTINGS_KEY_BRIGHTNESS,
                     (uint8_t)clamp_brightness(settings->brightness_percent));
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, CLOCK_SETTINGS_KEY_SLEEP, settings->sleep_timeout_sec);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, CLOCK_SETTINGS_KEY_WIFI, settings->wifi_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

