#include "touch_input.h"

#include "board_amoled_1_8.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch_input";

#define TOUCH_INPUT_POLL_MS 10
#define TOUCH_INPUT_MOVE_FLUSH_US (15 * 1000)
#define TOUCH_INPUT_TASK_STACK 4096
#define TOUCH_INPUT_TASK_PRIORITY 3

static touch_input_callback_t s_callback;
static void *s_callback_ctx;
static TaskHandle_t s_task;
static volatile bool s_hw_ready;

bool touch_input_hw_ready(void)
{
    return s_hw_ready;
}

static void touch_input_flush(touch_input_event_t *batch, size_t *count)
{
    if (*count == 0) {
        return;
    }
    s_callback(batch, *count, s_callback_ctx);
    *count = 0;
}

static void touch_input_append(touch_input_event_t *batch, size_t *count,
                               touch_input_event_kind_t kind, uint16_t x, uint16_t y)
{
    const touch_input_event_t event = {
        .kind = kind,
        .x = x,
        .y = y,
        .t_ms = (uint32_t)(esp_timer_get_time() / 1000),
    };
    if (kind == TOUCH_INPUT_EVENT_MOVE && *count > 0 &&
        batch[*count - 1].kind == TOUCH_INPUT_EVENT_MOVE) {
        batch[*count - 1] = event;
        return;
    }
    if (*count >= TOUCH_INPUT_MAX_BATCH) {
        touch_input_flush(batch, count);
    }
    batch[(*count)++] = event;
}

static void touch_input_task(void *arg)
{
    (void)arg;
    bool prev_pressed = false;
    uint16_t prev_x = 0;
    uint16_t prev_y = 0;
    touch_input_event_t batch[TOUCH_INPUT_MAX_BATCH];
    size_t batch_count = 0;
    int64_t batch_started_us = 0;
    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TOUCH_INPUT_POLL_MS));
        uint16_t x = 0;
        uint16_t y = 0;
        uint8_t pressed = 0;
        uint8_t point_num = 0;
        if (board_touch_read(&x, &y, &pressed, &point_num) != ESP_OK) {
            s_hw_ready = false;
            /* The driver lost the controller mid-touch; synthesize the release
             * so a subscriber never keeps a press latched. */
            if (prev_pressed) {
                touch_input_append(batch, &batch_count, TOUCH_INPUT_EVENT_UP, prev_x, prev_y);
                touch_input_flush(batch, &batch_count);
                prev_pressed = false;
            }
            continue;
        }
        s_hw_ready = true;
        const bool now_pressed = pressed != 0;
        const bool flush_due = batch_count > 0 &&
                               esp_timer_get_time() - batch_started_us >= TOUCH_INPUT_MOVE_FLUSH_US;
        if (!prev_pressed && now_pressed) {
            touch_input_append(batch, &batch_count, TOUCH_INPUT_EVENT_DOWN, x, y);
            touch_input_flush(batch, &batch_count);
            prev_x = x;
            prev_y = y;
        } else if (prev_pressed && now_pressed) {
            if (x != prev_x || y != prev_y) {
                if (batch_count == 0) {
                    batch_started_us = esp_timer_get_time();
                }
                touch_input_append(batch, &batch_count, TOUCH_INPUT_EVENT_MOVE, x, y);
                prev_x = x;
                prev_y = y;
            }
            if (flush_due) {
                touch_input_flush(batch, &batch_count);
            }
        } else if (prev_pressed && !now_pressed) {
            /* point_num carries the release marker with the last stable
             * coordinates; fall back to the previous sample otherwise. */
            const bool release_coords = point_num == BOARD_TOUCH_RELEASE_EVENT_POINT_NUM;
            touch_input_append(batch, &batch_count, TOUCH_INPUT_EVENT_UP,
                               release_coords ? x : prev_x, release_coords ? y : prev_y);
            touch_input_flush(batch, &batch_count);
        } else if (flush_due) {
            touch_input_flush(batch, &batch_count);
        }
        prev_pressed = now_pressed;
    }
}

esp_err_t touch_input_start(touch_input_callback_t callback, void *ctx)
{
    ESP_RETURN_ON_FALSE(callback != NULL, ESP_ERR_INVALID_ARG, TAG, "callback is null");
    ESP_RETURN_ON_FALSE(s_task == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_callback = callback;
    s_callback_ctx = ctx;
    ESP_RETURN_ON_FALSE(xTaskCreate(touch_input_task, "touch_input", TOUCH_INPUT_TASK_STACK,
                                    NULL, TOUCH_INPUT_TASK_PRIORITY, &s_task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "touch task");
    ESP_LOGI(TAG, "touch input task started: poll=%d ms", TOUCH_INPUT_POLL_MS);
    return ESP_OK;
}
