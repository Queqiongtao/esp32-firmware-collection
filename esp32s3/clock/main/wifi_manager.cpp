#include "wifi_manager.h"

#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "portmacro.h"

static const char *TAG = "WifiManager";

static constexpr char WIFI_NVS_NAMESPACE[] = "wifi_creds";
static_assert(sizeof(WIFI_NVS_NAMESPACE) <= NVS_NS_NAME_MAX_SIZE,
              "WiFi NVS namespace is too long");
static constexpr const char *WIFI_NVS_KEY_SSID = "ssid";
static constexpr const char *WIFI_NVS_KEY_PASSWORD = "pass";
static constexpr size_t WIFI_SSID_MAX_LEN = 32;
static constexpr size_t WIFI_PASSWORD_MAX_LEN = 64;
static constexpr uint64_t WIFI_RETRY_DELAY_US = 5ULL * 1000ULL * 1000ULL;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_manager_state_t s_state = WIFI_MANAGER_OFF;
static bool s_enabled = false;
static bool s_has_credentials = false;
static bool s_initialized = false;
static bool s_started = false;
static char s_ssid[WIFI_SSID_MAX_LEN + 1] = {};
static char s_password[WIFI_PASSWORD_MAX_LEN + 1] = {};
static char s_ip[16] = {};
static uint8_t s_disconnect_reason = 0;
static esp_netif_t *s_netif = nullptr;
static esp_timer_handle_t s_retry_timer = nullptr;

static void copy_string(char *dest, size_t dest_size, const char *src)
{
    if (dest == nullptr || dest_size == 0) {
        return;
    }
    size_t length = src != nullptr ? std::strlen(src) : 0;
    if (length >= dest_size) {
        length = dest_size - 1;
    }
    if (length > 0) {
        std::memcpy(dest, src, length);
    }
    dest[length] = '\0';
}

// WiFi config fields are fixed-size arrays and may legitimately use all bytes
// without a trailing NUL (SSID 32 bytes, password 64 bytes).
static void copy_fixed_string(uint8_t *dest, size_t dest_size, const char *src)
{
    if (dest == nullptr || dest_size == 0) {
        return;
    }
    size_t length = src != nullptr ? std::strlen(src) : 0;
    if (length > dest_size) {
        length = dest_size;
    }
    if (length > 0) {
        std::memcpy(dest, src, length);
    }
}

static void set_state(wifi_manager_state_t state)
{
    portENTER_CRITICAL(&s_lock);
    s_state = state;
    portEXIT_CRITICAL(&s_lock);
}

static void set_ip(const char *ip)
{
    portENTER_CRITICAL(&s_lock);
    if (ip == nullptr) {
        s_ip[0] = '\0';
    } else {
        copy_string(s_ip, sizeof(s_ip), ip);
    }
    portEXIT_CRITICAL(&s_lock);
}

static void set_disconnect_reason(uint8_t reason)
{
    portENTER_CRITICAL(&s_lock);
    s_disconnect_reason = reason;
    portEXIT_CRITICAL(&s_lock);
}

static bool get_credentials(char *ssid,
                            size_t ssid_size,
                            char *password,
                            size_t password_size)
{
    portENTER_CRITICAL(&s_lock);
    const bool has_credentials = s_has_credentials;
    if (ssid != nullptr && ssid_size > 0) {
        copy_string(ssid, ssid_size, s_ssid);
    }
    if (password != nullptr && password_size > 0) {
        copy_string(password, password_size, s_password);
    }
    portEXIT_CRITICAL(&s_lock);
    return has_credentials;
}

static void stop_retry_timer(void)
{
    if (s_retry_timer != nullptr) {
        esp_timer_stop(s_retry_timer);
    }
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (!wifi_manager_is_enabled() || !wifi_manager_has_credentials()) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    const bool already_connected = s_state == WIFI_MANAGER_CONNECTED;
    portEXIT_CRITICAL(&s_lock);
    if (already_connected) {
        return;
    }

    set_state(WIFI_MANAGER_CONNECTING);
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "WiFi retry failed: %s", esp_err_to_name(err));
    }
}

static void schedule_retry(void)
{
    if (s_retry_timer == nullptr) {
        return;
    }
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, WIFI_RETRY_DELAY_US);
}

static esp_err_t start_and_connect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    stop_retry_timer();
    set_disconnect_reason(0);

    if (!s_started) {
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
            ESP_LOGW(TAG, "WiFi start failed: %s", esp_err_to_name(err));
            set_state(WIFI_MANAGER_FAILED);
            return err;
        }
        s_started = true;
    }

    char ssid[WIFI_SSID_MAX_LEN + 1] = {};
    char password[WIFI_PASSWORD_MAX_LEN + 1] = {};
    if (!get_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
        set_state(WIFI_MANAGER_OFF);
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t config = {};
    copy_fixed_string(config.sta.ssid, sizeof(config.sta.ssid), ssid);
    copy_fixed_string(config.sta.password, sizeof(config.sta.password), password);
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi set config failed: %s", esp_err_to_name(err));
        set_state(WIFI_MANAGER_FAILED);
        return err;
    }

    esp_wifi_disconnect();
    set_state(WIFI_MANAGER_CONNECTING);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connect failed: %s", esp_err_to_name(err));
        set_state(WIFI_MANAGER_FAILED);
        schedule_retry();
        return err;
    }
    return ESP_OK;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        set_disconnect_reason(0);
        if (wifi_manager_is_enabled() && wifi_manager_has_credentials()) {
            set_state(WIFI_MANAGER_CONNECTING);
        } else {
            set_state(WIFI_MANAGER_OFF);
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        set_ip(nullptr);
        const auto *event = static_cast<const wifi_event_sta_disconnected_t *>(event_data);
        if (event != nullptr) {
            set_disconnect_reason(event->reason);
        }
        if (!wifi_manager_is_enabled()) {
            set_state(WIFI_MANAGER_OFF);
            return;
        }
        set_state(WIFI_MANAGER_FAILED);
        if (wifi_manager_has_credentials()) {
            ESP_LOGI(TAG,
                     "WiFi disconnected (reason=%u), retry in 5 seconds",
                     event != nullptr ? event->reason : 0);
            schedule_retry();
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (!wifi_manager_is_enabled()) {
            return;
        }
        const auto *event = static_cast<const ip_event_got_ip_t *>(event_data);
        char ip[16] = {};
        esp_ip4addr_ntoa(&event->ip_info.ip, ip, sizeof(ip));
        set_ip(ip);
        set_disconnect_reason(0);
        set_state(WIFI_MANAGER_CONNECTED);
        stop_retry_timer();
        ESP_LOGI(TAG, "WiFi connected, ip=%s", ip);
    }
}

static bool load_credentials_from_nvs(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Open WiFi NVS failed: %s", esp_err_to_name(err));
        return false;
    }

    char ssid[WIFI_SSID_MAX_LEN + 1] = {};
    char password[WIFI_PASSWORD_MAX_LEN + 1] = {};
    size_t ssid_size = sizeof(ssid);
    size_t password_size = sizeof(password);

    err = nvs_get_str(handle, WIFI_NVS_KEY_SSID, ssid, &ssid_size);
    if (err == ESP_OK) {
        esp_err_t password_err =
            nvs_get_str(handle, WIFI_NVS_KEY_PASSWORD, password, &password_size);
        if (password_err != ESP_OK && password_err != ESP_ERR_NVS_NOT_FOUND) {
            err = password_err;
        }
    }

    nvs_close(handle);
    if (err != ESP_OK || ssid[0] == '\0') {
        return false;
    }

    portENTER_CRITICAL(&s_lock);
    copy_string(s_ssid, sizeof(s_ssid), ssid);
    copy_string(s_password, sizeof(s_password), password);
    s_has_credentials = true;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

static esp_err_t save_credentials_to_nvs(const char *ssid, const char *password)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_NVS_KEY_PASSWORD, password != nullptr ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t wifi_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    load_credentials_from_nvs();

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "event loop create failed: %s", esp_err_to_name(err));
        return err;
    }

    if (s_netif == nullptr) {
        s_netif = esp_netif_create_default_wifi_sta();
        if (s_netif == nullptr) {
            ESP_LOGW(TAG, "Failed to create default WiFi STA netif");
            return ESP_FAIL;
        }
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_storage failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_register(WIFI_EVENT,
                                     ESP_EVENT_ANY_ID,
                                     &wifi_event_handler,
                                     nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi event handler register failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_register(IP_EVENT,
                                     IP_EVENT_STA_GOT_IP,
                                     &wifi_event_handler,
                                     nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IP event handler register failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = &retry_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_retry",
        .skip_unhandled_events = true,
    };
    err = esp_timer_create(&timer_args, &s_retry_timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create WiFi retry timer: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    set_disconnect_reason(0);
    set_state(WIFI_MANAGER_OFF);
    return ESP_OK;
}

esp_err_t wifi_manager_set_enabled(bool enabled)
{
    if (!s_initialized) {
        esp_err_t err = wifi_manager_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    if (!enabled) {
        portENTER_CRITICAL(&s_lock);
        s_enabled = false;
        portEXIT_CRITICAL(&s_lock);
        stop_retry_timer();
        esp_wifi_disconnect();
        if (s_started) {
            esp_wifi_stop();
            s_started = false;
        }
        set_ip(nullptr);
        set_disconnect_reason(0);
        set_state(WIFI_MANAGER_OFF);
        return ESP_OK;
    }

    if (!wifi_manager_has_credentials()) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    s_enabled = true;
    portEXIT_CRITICAL(&s_lock);
    return start_and_connect();
}

bool wifi_manager_is_enabled(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool enabled = s_enabled;
    portEXIT_CRITICAL(&s_lock);
    return enabled;
}

bool wifi_manager_has_credentials(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool has_credentials = s_has_credentials;
    portEXIT_CRITICAL(&s_lock);
    return has_credentials;
}

esp_err_t wifi_manager_scan(wifi_manager_ap_t *results, size_t max_results, size_t *count)
{
    if (results == nullptr || max_results == 0 || count == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;

    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) {
        return err;
    }

    bool started_here = false;
    if (!s_started) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            return err;
        }
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
            return err;
        }
        s_started = true;
        started_here = true;
    }

    wifi_scan_config_t config = {};
    err = esp_wifi_scan_start(&config, true);
    if (err == ESP_OK) {
        uint16_t found = 0;
        if (esp_wifi_scan_get_ap_num(&found) == ESP_OK && found > 0) {
            static wifi_ap_record_t records[16];
            uint16_t fetch_count = found > 16 ? 16 : found;
            if (esp_wifi_scan_get_ap_records(&fetch_count, records) == ESP_OK) {
                for (uint16_t i = 0; i < fetch_count && *count < max_results; ++i) {
                    if (records[i].ssid[0] == '\0') {
                        continue;
                    }
                    const char *ssid = reinterpret_cast<const char *>(records[i].ssid);
                    bool duplicate = false;
                    for (size_t j = 0; j < *count; ++j) {
                        if (std::strcmp(results[j].ssid, ssid) == 0) {
                            if (records[i].rssi > results[j].rssi) {
                                results[j].rssi = records[i].rssi;
                            }
                            duplicate = true;
                            break;
                        }
                    }
                    if (duplicate) {
                        continue;
                    }
                    copy_string(results[*count].ssid, sizeof(results[*count].ssid), ssid);
                    results[*count].rssi = records[i].rssi;
                    results[*count].secure = records[i].authmode != WIFI_AUTH_OPEN;
                    ++(*count);
                }
            }
        }
        esp_wifi_clear_ap_list();
    }

    if (started_here && !s_enabled) {
        esp_wifi_stop();
        s_started = false;
        set_state(WIFI_MANAGER_OFF);
    }

    ESP_LOGI(TAG, "WiFi scan found %u networks", (unsigned)*count);
    return err;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (ssid == nullptr || ssid[0] == '\0' ||
        std::strlen(ssid) > WIFI_SSID_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) {
        return err;
    }

    char saved_password[WIFI_PASSWORD_MAX_LEN + 1] = {};
    if (password == nullptr) {
        get_credentials(nullptr, 0, saved_password, sizeof(saved_password));
        password = saved_password;
    }
    if (std::strlen(password) > WIFI_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    err = save_credentials_to_nvs(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Save WiFi credentials failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "WiFi credentials saved, connecting to %s", ssid);

    portENTER_CRITICAL(&s_lock);
    copy_string(s_ssid, sizeof(s_ssid), ssid);
    copy_string(s_password, sizeof(s_password), password);
    s_has_credentials = true;
    s_enabled = true;
    portEXIT_CRITICAL(&s_lock);
    set_disconnect_reason(0);

    return start_and_connect();
}

esp_err_t wifi_manager_forget(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_erase_key(handle, WIFI_NVS_KEY_SSID);
        nvs_erase_key(handle, WIFI_NVS_KEY_PASSWORD);
        nvs_commit(handle);
        nvs_close(handle);
    }

    portENTER_CRITICAL(&s_lock);
    s_has_credentials = false;
    s_ssid[0] = '\0';
    s_password[0] = '\0';
    portEXIT_CRITICAL(&s_lock);

    wifi_manager_set_enabled(false);
    return ESP_OK;
}

void wifi_manager_get_status(wifi_manager_status_t *status)
{
    if (status == nullptr) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    status->state = s_state;
    status->enabled = s_enabled;
    status->has_credentials = s_has_credentials;
    status->disconnect_reason = s_disconnect_reason;
    status->rssi_valid = false;
    status->rssi = 0;
    copy_string(status->ssid, sizeof(status->ssid), s_ssid);
    copy_string(status->ip, sizeof(status->ip), s_ip);
    portEXIT_CRITICAL(&s_lock);

    if (status->state == WIFI_MANAGER_CONNECTED) {
        int rssi = 0;
        if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
            status->rssi = (int8_t)rssi;
            status->rssi_valid = true;
        }
    }
}
