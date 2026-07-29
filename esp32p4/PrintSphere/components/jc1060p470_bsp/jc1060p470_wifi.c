#include "bsp/jc1060p470.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "espressif__esp_hosted/host/esp_hosted.h"
#include "espressif__esp_hosted/host/esp_hosted_event.h"

static const char *TAG = "jc1060p470_wifi";
static bool s_ready;

#define HOSTED_TRANSPORT_UP_BIT BIT0
#define HOSTED_TRANSPORT_ERROR_BIT BIT1
#define HOSTED_READY_TIMEOUT_MS 5000

/* esp_hosted 2.12.x does not expose a public transport-ready getter. The
 * pinned private symbol handles a transport which was already connected. */
extern uint8_t is_transport_tx_ready(void);

typedef struct {
    EventGroupHandle_t event_group;
    int32_t last_event_id;
    bool cp_init_seen;
} hosted_wait_context_t;

static void hosted_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    (void)event_data;
    hosted_wait_context_t *context = (hosted_wait_context_t *)arg;
    if (event_base != ESP_HOSTED_EVENT || context == NULL || context->event_group == NULL) {
        return;
    }
    context->last_event_id = event_id;
    if (event_id == ESP_HOSTED_EVENT_CP_INIT) {
        context->cp_init_seen = true;
    }

    if (event_id == ESP_HOSTED_EVENT_TRANSPORT_UP) {
        xEventGroupSetBits(context->event_group, HOSTED_TRANSPORT_UP_BIT);
    } else if (event_id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE ||
               event_id == ESP_HOSTED_EVENT_TRANSPORT_DOWN) {
        xEventGroupSetBits(context->event_group, HOSTED_TRANSPORT_ERROR_BIT);
    }
}

esp_err_t bsp_wifi_hosted_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    EventGroupHandle_t event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(event_group != NULL, ESP_ERR_NO_MEM, TAG,
                        "create hosted event group failed");

    hosted_wait_context_t context = {
        .event_group = event_group,
        .last_event_id = -1,
        .cp_init_seen = false,
    };
    esp_event_handler_instance_t event_instance = NULL;
    ret = esp_event_handler_instance_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID,
                                              hosted_event_handler, &context,
                                              &event_instance);
    if (ret != ESP_OK) {
        vEventGroupDelete(event_group);
        return ret;
    }

    const int64_t wait_started_us = esp_timer_get_time();
    ret = esp_hosted_connect_to_slave();
    if (ret == ESP_OK && is_transport_tx_ready()) {
        xEventGroupSetBits(event_group, HOSTED_TRANSPORT_UP_BIT);
    }
    if (ret == ESP_OK) {
        const EventBits_t bits = xEventGroupWaitBits(
            event_group, HOSTED_TRANSPORT_UP_BIT | HOSTED_TRANSPORT_ERROR_BIT,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(HOSTED_READY_TIMEOUT_MS));
        if ((bits & HOSTED_TRANSPORT_ERROR_BIT) != 0U) {
            ret = ESP_FAIL;
        } else if ((bits & HOSTED_TRANSPORT_UP_BIT) == 0U) {
            ret = ESP_ERR_TIMEOUT;
        }
    }

    esp_event_handler_instance_unregister(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID,
                                          event_instance);
    const int64_t elapsed_ms = (esp_timer_get_time() - wait_started_us) / 1000;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Hosted not ready: err=%s elapsed=%lldms last_event=%ld cp_init=%d tx_ready=%u",
                 esp_err_to_name(ret), (long long)elapsed_ms, (long)context.last_event_id,
                 context.cp_init_seen ? 1 : 0, (unsigned)is_transport_tx_ready());
    }
    vEventGroupDelete(event_group);
    ESP_RETURN_ON_ERROR(ret, TAG, "ESP32-C6 hosted transport not ready");

    s_ready = true;
    ESP_LOGI(TAG, "ESP32-C6 hosted transport ready in %lldms (last_event=%ld cp_init=%d)",
             (long long)elapsed_ms, (long)context.last_event_id, context.cp_init_seen ? 1 : 0);
    return ESP_OK;
}
