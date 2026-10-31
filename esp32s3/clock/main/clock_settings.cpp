#include "clock_settings.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "ClockSettings";

static constexpr const char *CLOCK_SETTINGS_NAMESPACE = "clock_settings";
static constexpr const char *CLOCK_SETTINGS_KEY_BRIGHTNESS = "brightness";
static constexpr const char *CLOCK_SETTINGS_KEY_SLEEP = "sleep_sec";
static constexpr const char *CLOCK_SETTINGS_KEY_AUTO_ROTATE = "auto_rotate";
static constexpr const char *CLOCK_SETTINGS_KEY_WIFI = "wifi";
static constexpr const char *CLOCK_SETTINGS_KEY_NTP = "ntp";
static constexpr const char *CLOCK_SETTINGS_KEY_THEME = "theme_id";
static constexpr const char *CLOCK_SETTINGS_KEY_THEME_MAP = "theme_map";
static constexpr uint8_t CLOCK_SETTINGS_THEME_MAP_VERSION = 1;
static constexpr const char *CLOCK_SETTINGS_KEY_SOUND = "sound_on";
static constexpr const char *CLOCK_SETTINGS_KEY_VOLUME = "sound_vol";
static constexpr const char *CLOCK_SETTINGS_KEY_UI_SOUND = "ui_sound";
static constexpr const char *CLOCK_SETTINGS_KEY_CHIME = "chime_on";
static constexpr const char *CLOCK_SETTINGS_KEY_ALARMS = "alarm_cfg";

static constexpr uint8_t ALARM_BLOB_MAGIC = 0xA5;
static constexpr uint8_t ALARM_BLOB_VERSION = 1;

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t count;
    uint8_t reserved;
    clock_alarm_t alarms[CLOCK_ALARM_COUNT];
} alarm_blob_t;

static int clamp_volume(int volume)
{
    if (volume < 0) {
        return 0;
    }
    if (volume > 100) {
        return 100;
    }
    return volume;
}

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

static uint8_t migrate_theme_index(uint8_t theme_index)
{
    if (theme_index == 3) {
        // outline_analog was removed; analog is the closest remaining theme.
        return 2;
    }
    if (theme_index > 3 && theme_index <= 7) {
        return theme_index - 1;
    }
    return theme_index > 7 ? 0 : theme_index;
}

static void persist_theme_migration(uint8_t theme_index)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(CLOCK_SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, CLOCK_SETTINGS_KEY_THEME, theme_index);
        if (err == ESP_OK) {
            err = nvs_set_u8(handle,
                             CLOCK_SETTINGS_KEY_THEME_MAP,
                             CLOCK_SETTINGS_THEME_MAP_VERSION);
        }
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Persist theme migration failed: %s", esp_err_to_name(err));
    }
}

void clock_settings_defaults(clock_settings_t *settings)
{
    if (settings == nullptr) {
        return;
    }

    settings->brightness_percent = 70;
    settings->sleep_timeout_sec = 5 * 60;
    settings->auto_rotate = false;
    settings->wifi_enabled = false;
    settings->ntp_enabled = true;
    settings->theme_index = 0;
    settings->sound_enabled = true;
    settings->sound_volume = 30;
    settings->ui_sounds = true;
    settings->hourly_chime = false;
    for (size_t i = 0; i < CLOCK_ALARM_COUNT; ++i) {
        settings->alarms[i] = {};
    }
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

    uint8_t auto_rotate = 0;
    err = nvs_get_u8(handle, CLOCK_SETTINGS_KEY_AUTO_ROTATE, &auto_rotate);
    if (err == ESP_OK) {
        settings->auto_rotate = auto_rotate != 0;
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

    uint8_t ntp_enabled = 1;
    err = nvs_get_u8(handle, CLOCK_SETTINGS_KEY_NTP, &ntp_enabled);
    if (err == ESP_OK) {
        settings->ntp_enabled = ntp_enabled != 0;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return err;
    }

    uint8_t sound_enabled = 1;
    err = nvs_get_u8(handle, CLOCK_SETTINGS_KEY_SOUND, &sound_enabled);
    if (err == ESP_OK) {
        settings->sound_enabled = sound_enabled != 0;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return err;
    }

    uint8_t sound_volume = 30;
    err = nvs_get_u8(handle, CLOCK_SETTINGS_KEY_VOLUME, &sound_volume);
    if (err == ESP_OK) {
        settings->sound_volume = (uint8_t)clamp_volume(sound_volume);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return err;
    }

    uint8_t ui_sounds = 1;
    err = nvs_get_u8(handle, CLOCK_SETTINGS_KEY_UI_SOUND, &ui_sounds);
    if (err == ESP_OK) {
        settings->ui_sounds = ui_sounds != 0;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return err;
    }

    uint8_t hourly_chime = 0;
    err = nvs_get_u8(handle, CLOCK_SETTINGS_KEY_CHIME, &hourly_chime);
    if (err == ESP_OK) {
        settings->hourly_chime = hourly_chime != 0;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return err;
    }

    alarm_blob_t alarm_blob = {};
    size_t alarm_blob_size = sizeof(alarm_blob);
    err = nvs_get_blob(handle, CLOCK_SETTINGS_KEY_ALARMS, &alarm_blob, &alarm_blob_size);
    if (err == ESP_OK && alarm_blob_size == sizeof(alarm_blob) &&
        alarm_blob.magic == ALARM_BLOB_MAGIC &&
        alarm_blob.version == ALARM_BLOB_VERSION &&
        alarm_blob.count == CLOCK_ALARM_COUNT) {
        for (size_t i = 0; i < CLOCK_ALARM_COUNT; ++i) {
            settings->alarms[i] = alarm_blob.alarms[i];
        }
    } else if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return err;
    }

    uint8_t theme_map = 0;
    err = nvs_get_u8(handle, CLOCK_SETTINGS_KEY_THEME_MAP, &theme_map);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return err;
    }

    uint8_t theme_index = 0;
    bool theme_index_found = false;
    err = nvs_get_u8(handle, CLOCK_SETTINGS_KEY_THEME, &theme_index);
    if (err == ESP_OK) {
        settings->theme_index = theme_index;
        theme_index_found = true;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return err;
    }

    const bool migrate_theme =
        theme_index_found && theme_map < CLOCK_SETTINGS_THEME_MAP_VERSION;
    if (migrate_theme) {
        const uint8_t old_theme_index = settings->theme_index;
        settings->theme_index = migrate_theme_index(old_theme_index);
        ESP_LOGI(TAG,
                 "Migrate theme index %u -> %u",
                 (unsigned)old_theme_index,
                 (unsigned)settings->theme_index);
    }

    nvs_close(handle);
    if (migrate_theme) {
        persist_theme_migration(settings->theme_index);
    }
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
        err = nvs_set_u8(handle, CLOCK_SETTINGS_KEY_AUTO_ROTATE, settings->auto_rotate ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, CLOCK_SETTINGS_KEY_WIFI, settings->wifi_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, CLOCK_SETTINGS_KEY_NTP, settings->ntp_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle,
                         CLOCK_SETTINGS_KEY_SOUND,
                         settings->sound_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle,
                         CLOCK_SETTINGS_KEY_VOLUME,
                         (uint8_t)clamp_volume(settings->sound_volume));
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, CLOCK_SETTINGS_KEY_UI_SOUND, settings->ui_sounds ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, CLOCK_SETTINGS_KEY_CHIME, settings->hourly_chime ? 1 : 0);
    }
    if (err == ESP_OK) {
        alarm_blob_t alarm_blob = {};
        alarm_blob.magic = ALARM_BLOB_MAGIC;
        alarm_blob.version = ALARM_BLOB_VERSION;
        alarm_blob.count = CLOCK_ALARM_COUNT;
        for (size_t i = 0; i < CLOCK_ALARM_COUNT; ++i) {
            alarm_blob.alarms[i] = settings->alarms[i];
        }
        err = nvs_set_blob(handle, CLOCK_SETTINGS_KEY_ALARMS, &alarm_blob, sizeof(alarm_blob));
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, CLOCK_SETTINGS_KEY_THEME, settings->theme_index);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle,
                         CLOCK_SETTINGS_KEY_THEME_MAP,
                         CLOCK_SETTINGS_THEME_MAP_VERSION);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

