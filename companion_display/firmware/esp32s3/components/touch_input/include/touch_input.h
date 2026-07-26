#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOUCH_INPUT_MAX_BATCH 8

typedef enum {
    TOUCH_INPUT_EVENT_DOWN,
    TOUCH_INPUT_EVENT_MOVE,
    TOUCH_INPUT_EVENT_UP,
} touch_input_event_kind_t;

typedef struct {
    touch_input_event_kind_t kind;
    uint16_t x;
    uint16_t y;
    uint32_t t_ms;
} touch_input_event_t;

/* Called from the touch polling task; the events buffer is only valid during
 * the call. Batches are ordered and never exceed TOUCH_INPUT_MAX_BATCH. */
typedef void (*touch_input_callback_t)(const touch_input_event_t *events, size_t count, void *ctx);

esp_err_t touch_input_start(touch_input_callback_t callback, void *ctx);

/* Cached readiness from the polling task; never touches the I2C bus, so it is
 * safe to call from HTTP handlers while the touch task owns the driver. */
bool touch_input_hw_ready(void);

#ifdef __cplusplus
}
#endif
