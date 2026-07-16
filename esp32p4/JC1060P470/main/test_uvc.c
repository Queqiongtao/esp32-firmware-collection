/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "test_uvc.h"

#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_cache.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "test_config.h"
#include "driver/jpeg_decode.h"
#include "driver/jpeg_types.h"
#include "driver/ppa.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

static const char *TAG = "test_uvc";

#define UVC_STREAMING_MARKER 0x55564353U
static uint32_t RTC_NOINIT_ATTR s_uvc_streaming_marker;

bool test_uvc_should_resume_after_reset(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT) {
        s_uvc_streaming_marker = 0;
    }
    return s_uvc_streaming_marker == UVC_STREAMING_MARKER;
}

void test_uvc_clear_resume_marker(void)
{
    s_uvc_streaming_marker = 0;
}

#define UVC_DEVICE_CONNECTED_BIT BIT0
#define UVC_USB_ALL_FREE_BIT     BIT1
#define UVC_WORKER_EXITED_BIT    BIT2
#define UVC_USBLIB_EXITED_BIT    BIT3
#define UVC_TASK_STOP_TIMEOUT_MS 2000
/* Retired slots are not reused until uvc_host_uninstall() joins all callbacks.
 * Exhaustion is reported so the role recovery state machine escalates to STACK. */
#define UVC_STREAM_CONTEXT_POOL_SIZE 8
#define UVC_HELD_FRAME_SLOT_INVALID UINT8_MAX

typedef enum {
    UVC_APP_EVENT_FRAME = 0,
    UVC_APP_EVENT_DISCONNECTED,
    UVC_APP_EVENT_STOP,
} uvc_app_event_t;

typedef struct {
    uvc_app_event_t event;
    uvc_host_stream_hdl_t stream_hdl;
    uvc_host_frame_t *frame;
    uint32_t stream_generation;
    uint8_t held_slot;
} uvc_app_queue_msg_t;

typedef enum {
    UVC_STREAM_CONTEXT_FREE = 0,
    UVC_STREAM_CONTEXT_RESERVED,
    UVC_STREAM_CONTEXT_ACTIVE,
    UVC_STREAM_CONTEXT_RETIRED,
} uvc_stream_context_state_t;

typedef struct {
    uvc_host_stream_hdl_t stream_hdl;
    uint32_t generation;
    uvc_stream_context_state_t state;
} uvc_stream_context_t;

typedef enum {
    UVC_HELD_FRAME_FREE = 0,
    UVC_HELD_FRAME_QUEUED,
    UVC_HELD_FRAME_PROCESSING,
    UVC_HELD_FRAME_RETRY,
} uvc_held_frame_state_t;

typedef struct {
    uvc_held_frame_state_t state;
    uvc_app_queue_msg_t msg;
} uvc_held_frame_slot_t;

static QueueHandle_t s_event_queue;
static EventGroupHandle_t s_device_event_group;
static TaskHandle_t s_usblib_task_handle;
static TaskHandle_t s_worker_task_handle;
static jpeg_decoder_handle_t s_jpeg_decoder;
static ppa_client_handle_t s_ppa_srm;
static bool s_ppa_runtime_disabled;
static uvc_host_stream_hdl_t s_stream_hdl;
static uint8_t *s_rgb565_buf;
static size_t s_rgb565_buf_size;
static uint8_t *s_preview_buf;
static SemaphoreHandle_t s_preview_lock;
static SemaphoreHandle_t s_decode_lock;
static SemaphoreHandle_t s_stream_lock;
static uint32_t s_preview_frame_id;
static uint32_t s_preview_source_w;
static uint32_t s_preview_source_h;
static uint32_t s_preview_dst_w;
static uint32_t s_preview_dst_h;
static uint32_t s_preview_dst_x;
static uint32_t s_preview_dst_y;
static int64_t s_preview_last_update_us;

/* Device capability enumeration result, filled in the DEVICE_CONNECTED
 * callback and consumed by test_uvc_run() when opening the stream. */
static uvc_host_stream_format_t s_negotiated_format;
static bool s_format_negotiated;

/* Scratch buffer for uvc_host_get_frame_list(). Sized in test_config.h. */
static uvc_host_frame_info_t s_frame_info_list[TEST_UVC_FRAME_INFO_MAX];
static size_t s_frame_info_count;
static bool s_requested_resolution_valid;
static uint32_t s_requested_h_res = TEST_UVC_PREFER_H_RES;
static uint32_t s_requested_v_res = TEST_UVC_PREFER_V_RES;

static bool s_initialized;
static bool s_stopping;
static volatile bool s_worker_stop;
static volatile bool s_usblib_stop;
static bool s_usb_host_installed;
static bool s_uvc_driver_installed;
static bool s_waiting_for_all_free;
static bool s_controller_released = true;
static bool s_stream_opened;
static bool s_stream_started;
static bool s_device_connected;
static bool s_accept_frames;
static bool s_root_port_powered;
static portMUX_TYPE s_frame_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_frames_held_by_app;
static uint32_t s_stream_generation_counter;
static uvc_stream_context_t s_stream_context_pool[UVC_STREAM_CONTEXT_POOL_SIZE];
static uvc_stream_context_t *s_stream_context;
static uvc_held_frame_slot_t s_held_frame_slots[TEST_UVC_FRAME_BUF_COUNT];
static bool s_disconnect_pending;
static uvc_host_stream_hdl_t s_disconnect_stream_hdl;
static uint32_t s_disconnect_stream_generation;

static uint32_t s_frames_received;
static uint32_t s_frames_decoded;
static uint32_t s_decode_errors;
static uint32_t s_frames_dropped;
static uint32_t s_frame_queue_high_water;
static uint32_t s_last_frame_bytes;
static uint32_t s_last_decode_us;
static uint32_t s_last_preview_us;
static uint32_t s_last_frame_w;
static uint32_t s_last_frame_h;
static int64_t s_last_frame_time_us;
static test_uvc_state_t s_state = TEST_UVC_STATE_IDLE;
static test_uvc_recovery_level_t s_recovery_level = TEST_UVC_RECOVERY_NONE;
static uint32_t s_recovery_attempts;
static bool s_recovery_required;
static test_uvc_preview_backend_t s_preview_backend = TEST_UVC_PREVIEW_BACKEND_CPU;
static uint32_t s_preview_fallbacks;
static uint32_t s_transfer_errors;
static uint32_t s_frame_overflows;
static uint32_t s_frame_underflows;
static uint32_t s_usblib_errors;
static esp_err_t s_last_error = ESP_OK;
static test_module_meta_t s_module = {
    .state = TEST_MODULE_STATE_UNINITIALIZED,
    .last_error = ESP_OK,
};
static int64_t s_stats_window_start_us;
static uint32_t s_stats_window_received;
static uint32_t s_stats_window_decoded;
static uint32_t s_stats_window_errors;

static bool uvc_stream_lock_take(TickType_t ticks)
{
    return s_stream_lock != NULL && xSemaphoreTakeRecursive(s_stream_lock, ticks) == pdTRUE;
}

static bool uvc_task_running(TaskHandle_t *task_handle)
{
    portENTER_CRITICAL(&s_frame_lock);
    bool running = *task_handle != NULL;
    portEXIT_CRITICAL(&s_frame_lock);
    return running;
}

static esp_err_t uvc_wait_task_stopped(TaskHandle_t *task_handle)
{
    const int64_t deadline_us = esp_timer_get_time() + UVC_TASK_STOP_TIMEOUT_MS * 1000LL;
    while (uvc_task_running(task_handle)) {
        if (esp_timer_get_time() >= deadline_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return ESP_OK;
}

static void uvc_stream_lock_give(void)
{
    if (s_stream_lock != NULL) {
        (void)xSemaphoreGiveRecursive(s_stream_lock);
    }
}

static esp_err_t uvc_ensure_stream_lock(void)
{
    portENTER_CRITICAL(&s_frame_lock);
    const bool lock_exists = s_stream_lock != NULL;
    portEXIT_CRITICAL(&s_frame_lock);
    if (lock_exists) {
        return ESP_OK;
    }

    SemaphoreHandle_t new_lock = xSemaphoreCreateRecursiveMutex();
    if (new_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_frame_lock);
    if (s_stream_lock == NULL) {
        s_stream_lock = new_lock;
        new_lock = NULL;
    }
    portEXIT_CRITICAL(&s_frame_lock);
    if (new_lock != NULL) {
        vSemaphoreDelete(new_lock);
    }
    return ESP_OK;
}

static uint32_t uvc_next_stream_generation_locked(void)
{
    uint32_t generation = ++s_stream_generation_counter;
    if (generation == 0) {
        generation = ++s_stream_generation_counter;
    }
    return generation;
}

static uvc_stream_context_t *uvc_reserve_stream_context(void)
{
    uvc_stream_context_t *context = NULL;
    portENTER_CRITICAL(&s_frame_lock);
    for (size_t i = 0; i < UVC_STREAM_CONTEXT_POOL_SIZE; ++i) {
        if (s_stream_context_pool[i].state == UVC_STREAM_CONTEXT_FREE) {
            context = &s_stream_context_pool[i];
            *context = (uvc_stream_context_t) {
                .generation = uvc_next_stream_generation_locked(),
                .state = UVC_STREAM_CONTEXT_RESERVED,
            };
            break;
        }
    }
    portEXIT_CRITICAL(&s_frame_lock);
    return context;
}

static void uvc_release_reserved_stream_context(uvc_stream_context_t *context)
{
    if (context == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_frame_lock);
    if (context->state == UVC_STREAM_CONTEXT_RESERVED && context != s_stream_context) {
        *context = (uvc_stream_context_t) {0};
    }
    portEXIT_CRITICAL(&s_frame_lock);
}

static void uvc_publish_stream_context(uvc_stream_context_t *context,
                                       uvc_host_stream_hdl_t stream_hdl)
{
    assert(context != NULL);
    assert(stream_hdl != NULL);
    portENTER_CRITICAL(&s_frame_lock);
    assert(context->state == UVC_STREAM_CONTEXT_RESERVED);
    assert(s_stream_context == NULL);
    context->stream_hdl = stream_hdl;
    context->state = UVC_STREAM_CONTEXT_ACTIVE;
    s_stream_context = context;
    portEXIT_CRITICAL(&s_frame_lock);
}

static bool uvc_active_stream_matches_locked(uvc_host_stream_hdl_t stream_hdl,
                                             uint32_t generation)
{
    return s_stream_context != NULL &&
           s_stream_context->state == UVC_STREAM_CONTEXT_ACTIVE &&
           s_stream_context->stream_hdl == stream_hdl &&
           s_stream_context->generation == generation;
}

static void uvc_retire_stream_context(uvc_stream_context_t *context)
{
    if (context == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_frame_lock);
    if (context == s_stream_context && context->state == UVC_STREAM_CONTEXT_ACTIVE) {
        if (s_disconnect_stream_hdl == context->stream_hdl &&
            s_disconnect_stream_generation == context->generation) {
            s_disconnect_pending = false;
            s_disconnect_stream_hdl = NULL;
            s_disconnect_stream_generation = 0;
        }
        s_stream_context = NULL;
        context->state = UVC_STREAM_CONTEXT_RETIRED;
    }
    portEXIT_CRITICAL(&s_frame_lock);
}

static void uvc_reset_stream_context_pool(void)
{
    portENTER_CRITICAL(&s_frame_lock);
    s_stream_context = NULL;
    memset(s_stream_context_pool, 0, sizeof(s_stream_context_pool));
    portEXIT_CRITICAL(&s_frame_lock);
}

static bool uvc_held_frame_matches_locked(const uvc_held_frame_slot_t *slot,
                                          const uvc_app_queue_msg_t *msg)
{
    return slot != NULL && msg != NULL &&
           msg->held_slot < TEST_UVC_FRAME_BUF_COUNT &&
           slot == &s_held_frame_slots[msg->held_slot] &&
           slot->state != UVC_HELD_FRAME_FREE &&
           slot->msg.stream_hdl == msg->stream_hdl &&
           slot->msg.stream_generation == msg->stream_generation &&
           slot->msg.frame == msg->frame;
}

static bool uvc_hold_frame_locked(uvc_host_stream_hdl_t stream_hdl,
                                  uint32_t stream_generation,
                                  uvc_host_frame_t *frame,
                                  uvc_app_queue_msg_t *out_msg)
{
    for (uint8_t i = 0; i < TEST_UVC_FRAME_BUF_COUNT; ++i) {
        uvc_held_frame_slot_t *slot = &s_held_frame_slots[i];
        if (slot->state != UVC_HELD_FRAME_FREE) {
            continue;
        }
        const uvc_app_queue_msg_t msg = {
            .event = UVC_APP_EVENT_FRAME,
            .stream_hdl = stream_hdl,
            .frame = frame,
            .stream_generation = stream_generation,
            .held_slot = i,
        };
        slot->state = UVC_HELD_FRAME_QUEUED;
        slot->msg = msg;
        s_frames_held_by_app++;
        *out_msg = msg;
        return true;
    }
    return false;
}

static void uvc_cancel_held_frame(const uvc_app_queue_msg_t *msg)
{
    if (msg == NULL || msg->held_slot >= TEST_UVC_FRAME_BUF_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_frame_lock);
    uvc_held_frame_slot_t *slot = &s_held_frame_slots[msg->held_slot];
    if (uvc_held_frame_matches_locked(slot, msg) && slot->state == UVC_HELD_FRAME_QUEUED) {
        *slot = (uvc_held_frame_slot_t) {0};
        if (s_frames_held_by_app > 0) {
            s_frames_held_by_app--;
        }
    }
    portEXIT_CRITICAL(&s_frame_lock);
}

static bool uvc_claim_held_frame(const uvc_app_queue_msg_t *msg)
{
    if (msg == NULL || msg->held_slot >= TEST_UVC_FRAME_BUF_COUNT) {
        return false;
    }
    bool claimed = false;
    portENTER_CRITICAL(&s_frame_lock);
    uvc_held_frame_slot_t *slot = &s_held_frame_slots[msg->held_slot];
    if (uvc_held_frame_matches_locked(slot, msg) && slot->state == UVC_HELD_FRAME_QUEUED) {
        slot->state = UVC_HELD_FRAME_PROCESSING;
        claimed = true;
    }
    portEXIT_CRITICAL(&s_frame_lock);
    return claimed;
}

static void uvc_reset_held_frames(void)
{
    portENTER_CRITICAL(&s_frame_lock);
    memset(s_held_frame_slots, 0, sizeof(s_held_frame_slots));
    s_frames_held_by_app = 0;
    portEXIT_CRITICAL(&s_frame_lock);
}

static void uvc_set_module_state(test_module_state_t state, esp_err_t error, bool new_generation)
{
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_frame_lock);
    if (new_generation) {
        s_module.generation++;
    }
    s_module.state = state;
    s_module.last_error = error;
    s_module.updated_at_us = now_us;
    s_last_error = error;
    portEXIT_CRITICAL(&s_frame_lock);
}

static bool uvc_should_sample_error(uint32_t count)
{
    return count <= 3 || (count & (count - 1U)) == 0;
}

static void uvc_set_state(test_uvc_state_t state, esp_err_t error)
{
    portENTER_CRITICAL(&s_frame_lock);
    s_state = state;
    if (error != ESP_OK) {
        s_last_error = error;
    }
    portEXIT_CRITICAL(&s_frame_lock);

    if (state == TEST_UVC_STATE_FAILED) {
        uvc_set_module_state(TEST_MODULE_STATE_ERROR, error, false);
    } else if (state == TEST_UVC_STATE_STREAMING) {
        uvc_set_module_state(TEST_MODULE_STATE_RUNNING, ESP_OK, false);
    } else if (state == TEST_UVC_STATE_HOST_READY) {
        uvc_set_module_state(TEST_MODULE_STATE_READY, error, false);
    } else if (state == TEST_UVC_STATE_ENUMERATING || state == TEST_UVC_STATE_RECOVERING) {
        uvc_set_module_state(TEST_MODULE_STATE_INITIALIZING, error, false);
    }
}

static const char *uvc_state_name(test_uvc_state_t state)
{
    switch (state) {
    case TEST_UVC_STATE_IDLE: return "idle";
    case TEST_UVC_STATE_HOST_READY: return "host-ready";
    case TEST_UVC_STATE_ENUMERATING: return "enumerating";
    case TEST_UVC_STATE_STREAMING: return "streaming";
    case TEST_UVC_STATE_RECOVERING: return "recovering";
    case TEST_UVC_STATE_FAILED: return "failed";
    default: return "unknown";
    }
}

static void uvc_log_memory(const char *stage)
{
    ESP_LOGI(TAG,
             "%s memory: dma_internal_free=%zu dma_internal_biggest=%zu internal_free=%zu spiram_free=%zu",
             stage,
             heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void uvc_log_stats_if_due(void)
{
    int64_t now_us = esp_timer_get_time();
    if (s_stats_window_start_us == 0) {
        s_stats_window_start_us = now_us;
        return;
    }

    if (now_us - s_stats_window_start_us < 5000000) {
        return;
    }

    ESP_LOGI(TAG, "UVC stats: state=%s received=%" PRIu32 " decoded=%" PRIu32 " errors=%" PRIu32
             " dropped=%" PRIu32 " last=%" PRIu32 "x%" PRIu32 " len=%" PRIu32
             " decode=%" PRIu32 "us preview=%" PRIu32 "us backend=%s",
             uvc_state_name(s_state),
             s_frames_received - s_stats_window_received,
             s_frames_decoded - s_stats_window_decoded,
             s_decode_errors - s_stats_window_errors,
             s_frames_dropped, s_last_frame_w, s_last_frame_h, s_last_frame_bytes,
             s_last_decode_us, s_last_preview_us,
             s_preview_backend == TEST_UVC_PREVIEW_BACKEND_PPA ? "PPA" : "CPU");

    s_stats_window_start_us = now_us;
    s_stats_window_received = s_frames_received;
    s_stats_window_decoded = s_frames_decoded;
    s_stats_window_errors = s_decode_errors;
}

/* Convert a UVC frame interval (100ns units) to frames-per-second. */
static float uvc_interval_to_fps(uint32_t interval_100ns)
{
    if (interval_100ns == 0) {
        return 0.0f;
    }
    return 10000000.0f / (float)interval_100ns;
}

/* Pick the highest-fps (smallest interval) entry from a uvc_host_frame_info_t.
 * For continuous ranges (interval_type == 0) use interval_min; for discrete
 * intervals use the minimum of interval[0..interval_type-1]. */
static uint32_t uvc_pick_best_interval(const uvc_host_frame_info_t *info)
{
    if (info->interval_type == 0) {
        return info->interval_min;
    }
    uint32_t best = info->interval[0];
    for (uint8_t i = 1; i < info->interval_type && i < CONFIG_UVC_INTERVAL_ARRAY_SIZE; ++i) {
        if (info->interval[i] < best) {
            best = info->interval[i];
        }
    }
    return best;
}

static esp_err_t uvc_ensure_rgb565_buf(uint32_t width, uint32_t height);

/* Enumerate into a local snapshot, then publish it atomically for readers. */
static esp_err_t uvc_negotiate_format(uint8_t dev_addr, uint8_t uvc_stream_index)
{
    uvc_host_frame_info_t frame_info_list[TEST_UVC_FRAME_INFO_MAX] = {0};
    bool requested_resolution_valid;
    uint32_t requested_h_res;
    uint32_t requested_v_res;

    portENTER_CRITICAL(&s_frame_lock);
    requested_resolution_valid = s_requested_resolution_valid;
    requested_h_res = s_requested_h_res;
    requested_v_res = s_requested_v_res;
    portEXIT_CRITICAL(&s_frame_lock);

    size_t list_size = TEST_UVC_FRAME_INFO_MAX;
    esp_err_t ret = uvc_host_get_frame_list(dev_addr, uvc_stream_index,
                                            &frame_info_list, &list_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "uvc_host_get_frame_list failed: %s (dev_addr=%u stream=%u)",
                 esp_err_to_name(ret), (unsigned)dev_addr, (unsigned)uvc_stream_index);
        portENTER_CRITICAL(&s_frame_lock);
        s_format_negotiated = false;
        s_frame_info_count = 0;
        portEXIT_CRITICAL(&s_frame_lock);
        return ret;
    }

    if (list_size == 0) {
        ESP_LOGW(TAG, "device reported 0 frame descriptors");
        portENTER_CRITICAL(&s_frame_lock);
        s_format_negotiated = false;
        s_frame_info_count = 0;
        portEXIT_CRITICAL(&s_frame_lock);
        return ESP_ERR_NOT_FOUND;
    }

    if (list_size > TEST_UVC_FRAME_INFO_MAX) {
        ESP_LOGW(TAG, "device reported %zu frame descriptors, only %d enumerated",
                 list_size, TEST_UVC_FRAME_INFO_MAX);
        list_size = TEST_UVC_FRAME_INFO_MAX;
    }
    ESP_LOGI(TAG, "enumerated %zu frame descriptors:", list_size);
    for (size_t i = 0; i < list_size; ++i) {
        const uvc_host_frame_info_t *f = &frame_info_list[i];
        ESP_LOGI(TAG, "  [%zu] format=%d %ux%u interval_type=%u",
                 i, (int)f->format, (unsigned)f->h_res, (unsigned)f->v_res,
                 (unsigned)f->interval_type);
    }

    /* Pass 1: MJPEG + preferred resolution + highest fps */
    const uvc_host_frame_info_t *best_preferred = NULL;
    uint32_t best_preferred_interval = 0;
    /* Pass 2: MJPEG + largest resolution + highest fps (fallback) */
    const uvc_host_frame_info_t *best_mjpeg = NULL;
    uint32_t best_mjpeg_interval = 0;
    uint32_t best_mjpeg_pixels = 0;

    for (size_t i = 0; i < list_size; ++i) {
        const uvc_host_frame_info_t *f = &frame_info_list[i];
        if (f->format != TEST_UVC_FORMAT) {
            continue;
        }

        uint32_t interval = uvc_pick_best_interval(f);
        uint32_t pixels = f->h_res * f->v_res;

        uint32_t preferred_h_res = requested_resolution_valid ? requested_h_res : TEST_UVC_PREFER_H_RES;
        uint32_t preferred_v_res = requested_resolution_valid ? requested_v_res : TEST_UVC_PREFER_V_RES;
        if (f->h_res == preferred_h_res && f->v_res == preferred_v_res) {
            if (best_preferred == NULL || interval < best_preferred_interval) {
                best_preferred = f;
                best_preferred_interval = interval;
            }
        }

        if (best_mjpeg == NULL || pixels > best_mjpeg_pixels ||
            (pixels == best_mjpeg_pixels && interval < best_mjpeg_interval)) {
            best_mjpeg = f;
            best_mjpeg_interval = interval;
            best_mjpeg_pixels = pixels;
        }
    }

    const uvc_host_frame_info_t *chosen = best_preferred;
    uint32_t chosen_interval = best_preferred_interval;
    if (chosen == NULL) {
        chosen = best_mjpeg;
        chosen_interval = best_mjpeg_interval;
    }

    if (chosen == NULL) {
        ESP_LOGE(TAG, "camera exposes no MJPEG frame descriptor");
        portENTER_CRITICAL(&s_frame_lock);
        memcpy(s_frame_info_list, frame_info_list,
               list_size * sizeof(s_frame_info_list[0]));
        s_frame_info_count = list_size;
        s_format_negotiated = false;
        portEXIT_CRITICAL(&s_frame_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uvc_host_stream_format_t negotiated_format = {
        .h_res = chosen->h_res,
        .v_res = chosen->v_res,
        .fps = uvc_interval_to_fps(chosen_interval),
        .format = chosen->format,
    };
    const bool preferred = chosen == best_preferred;

    portENTER_CRITICAL(&s_frame_lock);
    memcpy(s_frame_info_list, frame_info_list,
           list_size * sizeof(s_frame_info_list[0]));
    s_frame_info_count = list_size;
    s_negotiated_format = negotiated_format;
    s_format_negotiated = true;
    portEXIT_CRITICAL(&s_frame_lock);

    ESP_LOGI(TAG, "negotiated format: %ux%u@%.1ffps format=%d%s",
             (unsigned)negotiated_format.h_res, (unsigned)negotiated_format.v_res,
             negotiated_format.fps, (int)negotiated_format.format,
             preferred ? " (preferred)" : " (fallback)");
    return ESP_OK;
}

static esp_err_t uvc_update_preview_ppa(uint32_t src_w, uint32_t src_h, size_t source_stride,
                                        uint32_t dst_w, uint32_t dst_h,
                                        uint32_t dst_x, uint32_t dst_y)
{
    const ppa_srm_oper_config_t config = {
        .in = {
            .buffer = s_rgb565_buf,
            .pic_w = source_stride,
            .pic_h = src_h,
            .block_w = src_w,
            .block_h = src_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = s_preview_buf,
            .buffer_size = TEST_UVC_PREVIEW_H_RES * TEST_UVC_PREVIEW_V_RES * sizeof(uint16_t),
            .pic_w = TEST_UVC_PREVIEW_H_RES,
            .pic_h = TEST_UVC_PREVIEW_V_RES,
            .block_offset_x = dst_x,
            .block_offset_y = dst_y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = (float)dst_w / (float)src_w,
        .scale_y = (float)dst_h / (float)src_h,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_ppa_srm, &config);
}

static void uvc_update_preview_cpu(uint32_t src_w, uint32_t src_h, size_t source_stride,
                                   uint32_t dst_w, uint32_t dst_h,
                                   uint32_t dst_x, uint32_t dst_y)
{
    const uint16_t *src = (const uint16_t *)s_rgb565_buf;
    uint16_t *dst = (uint16_t *)s_preview_buf;
    for (uint32_t y = 0; y < dst_h; ++y) {
        const uint32_t src_y = (uint32_t)(((uint64_t)y * src_h) / dst_h);
        uint16_t *dst_row = dst + ((dst_y + y) * TEST_UVC_PREVIEW_H_RES) + dst_x;
        const uint16_t *src_row = src + (src_y * source_stride);
        for (uint32_t x = 0; x < dst_w; ++x) {
            dst_row[x] = src_row[((uint64_t)x * src_w) / dst_w];
        }
    }
}

static void uvc_update_preview(uint32_t src_w, uint32_t src_h, uint32_t out_size)
{
    if (s_preview_buf == NULL || s_preview_lock == NULL || src_w == 0 || src_h == 0) {
        return;
    }

    const uint32_t aligned_w = (src_w + 15U) & ~15U;
    const uint32_t aligned_h = (src_h + 15U) & ~15U;
    const size_t expected_size = (size_t)aligned_w * aligned_h * sizeof(uint16_t);
    const size_t source_stride = aligned_w;
    if (aligned_w < src_w || aligned_h < src_h || out_size < expected_size) {
        ESP_LOGW(TAG, "decoded RGB565 layout invalid: source=%ux%u aligned=%ux%u out=%u expected=%zu",
                 (unsigned)src_w, (unsigned)src_h, (unsigned)aligned_w, (unsigned)aligned_h,
                 (unsigned)out_size, expected_size);
        return;
    }

    uint32_t dst_w = TEST_UVC_PREVIEW_H_RES;
    uint32_t dst_h = (uint32_t)(((uint64_t)src_h * dst_w) / src_w);
    if (dst_h > TEST_UVC_PREVIEW_V_RES) {
        dst_h = TEST_UVC_PREVIEW_V_RES;
        dst_w = (uint32_t)(((uint64_t)src_w * dst_h) / src_h);
    }
    const uint32_t dst_x = (TEST_UVC_PREVIEW_H_RES - dst_w) / 2U;
    const uint32_t dst_y = (TEST_UVC_PREVIEW_V_RES - dst_h) / 2U;

    if (xSemaphoreTake(s_preview_lock, 0) != pdTRUE) {
        return;
    }

    bool preview_cleared = false;
    if (s_preview_frame_id == 0 || dst_w != s_preview_dst_w || dst_h != s_preview_dst_h ||
        dst_x != s_preview_dst_x || dst_y != s_preview_dst_y) {
        memset(s_preview_buf, 0, TEST_UVC_PREVIEW_H_RES * TEST_UVC_PREVIEW_V_RES * 2U);
        preview_cleared = true;
        s_preview_dst_w = dst_w;
        s_preview_dst_h = dst_h;
        s_preview_dst_x = dst_x;
        s_preview_dst_y = dst_y;
    }
    const int64_t start_us = esp_timer_get_time();
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    if (s_ppa_srm != NULL && !s_ppa_runtime_disabled) {
        if (preview_cleared) {
            /* The PPA driver invalidates its output window before DMA. Commit
             * CPU-written black bars first so that invalidation cannot discard them. */
            esp_cache_msync(s_preview_buf, TEST_UVC_PREVIEW_H_RES * TEST_UVC_PREVIEW_V_RES * 2U,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
        ret = uvc_update_preview_ppa(src_w, src_h, source_stride,
                                     dst_w, dst_h, dst_x, dst_y);
    }
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_frame_lock);
        s_preview_backend = TEST_UVC_PREVIEW_BACKEND_PPA;
        portEXIT_CRITICAL(&s_frame_lock);
    } else {
        portENTER_CRITICAL(&s_frame_lock);
        s_preview_backend = TEST_UVC_PREVIEW_BACKEND_CPU;
        if (s_ppa_srm != NULL && !s_ppa_runtime_disabled) {
            s_ppa_runtime_disabled = true;
            s_preview_fallbacks++;
            portEXIT_CRITICAL(&s_frame_lock);
            ESP_LOGW(TAG, "PPA preview scaling failed, lock CPU fallback until UVC restart: %s",
                     esp_err_to_name(ret));
        } else {
            portEXIT_CRITICAL(&s_frame_lock);
        }
        uvc_update_preview_cpu(src_w, src_h, source_stride, dst_w, dst_h, dst_x, dst_y);
    }
    esp_cache_msync(s_preview_buf, TEST_UVC_PREVIEW_H_RES * TEST_UVC_PREVIEW_V_RES * 2U,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    portENTER_CRITICAL(&s_frame_lock);
    s_last_preview_us = (uint32_t)(esp_timer_get_time() - start_us);
    s_preview_source_w = src_w;
    s_preview_source_h = src_h;
    s_preview_frame_id++;
    portEXIT_CRITICAL(&s_frame_lock);
    xSemaphoreGive(s_preview_lock);
}

static void uvc_driver_event_callback(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;

    switch (event->type) {
    case UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED:
        portENTER_CRITICAL(&s_frame_lock);
        s_device_connected = true;
        portEXIT_CRITICAL(&s_frame_lock);
        ESP_LOGI(TAG, "UVC driver: compatible device connected, dev_addr=%u stream_index=%u frame_info_num=%u",
                 event->device_connected.dev_addr,
                 event->device_connected.uvc_stream_index,
                 (unsigned)event->device_connected.frame_info_num);
        uvc_negotiate_format(event->device_connected.dev_addr,
                             event->device_connected.uvc_stream_index);
        xEventGroupSetBits(s_device_event_group, UVC_DEVICE_CONNECTED_BIT);
        portENTER_CRITICAL(&s_frame_lock);
        const bool stream_started = s_stream_started;
        portEXIT_CRITICAL(&s_frame_lock);
        if (!stream_started) {
            uvc_set_state(TEST_UVC_STATE_HOST_READY, ESP_OK);
        }
        break;
    default:
        ESP_LOGW(TAG, "UVC driver: unknown event %d", event->type);
        break;
    }
}

static void uvc_stream_event_callback(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    const uvc_stream_context_t *context = (const uvc_stream_context_t *)user_ctx;
    portENTER_CRITICAL(&s_frame_lock);
    const bool active_context = context != NULL && context == s_stream_context &&
                                context->state == UVC_STREAM_CONTEXT_ACTIVE &&
                                context->stream_hdl != NULL && context->generation != 0;
    portEXIT_CRITICAL(&s_frame_lock);
    if (!active_context) {
        return;
    }

    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        portENTER_CRITICAL(&s_frame_lock);
        s_last_error = event->transfer_error.error;
        s_transfer_errors++;
        uint32_t transfer_errors = s_transfer_errors;
        portEXIT_CRITICAL(&s_frame_lock);
        if (uvc_should_sample_error(transfer_errors)) {
            ESP_LOGW(TAG, "UVC transfer errors=%" PRIu32 ", latest=%s",
                     transfer_errors, esp_err_to_name(event->transfer_error.error));
        }
        break;
    case UVC_HOST_DEVICE_DISCONNECTED: {
        uvc_host_stream_hdl_t hdl = event->device_disconnected.stream_hdl;
        uint32_t generation = 0;
        portENTER_CRITICAL(&s_frame_lock);
        generation = context == s_stream_context &&
                     context->state == UVC_STREAM_CONTEXT_ACTIVE &&
                     context->stream_hdl == hdl ? context->generation : 0;
        if (generation != 0) {
            s_disconnect_pending = true;
            s_disconnect_stream_hdl = hdl;
            s_disconnect_stream_generation = generation;
            s_accept_frames = false;
            s_device_connected = false;
            s_format_negotiated = false;
            s_frame_info_count = 0;
            s_recovery_required = false;
        }
        portEXIT_CRITICAL(&s_frame_lock);
        ESP_LOGI(TAG, "UVC device disconnected, scheduling stream cleanup (generation=%" PRIu32 ")",
                 generation);
        if (generation != 0 && s_device_event_group != NULL) {
            xEventGroupClearBits(s_device_event_group, UVC_DEVICE_CONNECTED_BIT);
        }
        const uvc_app_queue_msg_t msg = {
            .event = UVC_APP_EVENT_DISCONNECTED,
            .stream_hdl = hdl,
            .frame = NULL,
            .stream_generation = generation,
            .held_slot = UVC_HELD_FRAME_SLOT_INVALID,
        };
        /* The pending flag is the fallback when the frame queue is full. */
        if (generation != 0 && s_event_queue != NULL &&
            xQueueSendToFront(s_event_queue, &msg, 0) != pdTRUE) {
            ESP_LOGW(TAG, "UVC disconnect control queue full; worker will poll pending state");
        }
        break;
    }
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        portENTER_CRITICAL(&s_frame_lock);
        s_frame_overflows++;
        uint32_t frame_overflows = s_frame_overflows;
        portEXIT_CRITICAL(&s_frame_lock);
        if (uvc_should_sample_error(frame_overflows)) {
            ESP_LOGW(TAG, "UVC frame buffer overflows=%" PRIu32, frame_overflows);
        }
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        portENTER_CRITICAL(&s_frame_lock);
        s_frame_underflows++;
        uint32_t frame_underflows = s_frame_underflows;
        portEXIT_CRITICAL(&s_frame_lock);
        if (uvc_should_sample_error(frame_underflows)) {
            ESP_LOGW(TAG, "UVC frame buffer underflows=%" PRIu32, frame_underflows);
        }
        break;
    default:
        ESP_LOGW(TAG, "UVC stream: unsupported event %d (possibly suspend/resume)", event->type);
        break;
    }
}

static bool uvc_frame_callback(const uvc_host_frame_t *frame, void *user_ctx)
{
    const uvc_stream_context_t *context = (const uvc_stream_context_t *)user_ctx;
    if (context == NULL || s_event_queue == NULL) {
        return true;
    }

    portENTER_CRITICAL(&s_frame_lock);
    if (s_stopping || !s_accept_frames || context != s_stream_context ||
        context->state != UVC_STREAM_CONTEXT_ACTIVE ||
        context->stream_hdl == NULL || context->generation == 0) {
        portEXIT_CRITICAL(&s_frame_lock);
        return true;
    }
    uvc_app_queue_msg_t msg = {0};
    if (!uvc_hold_frame_locked(context->stream_hdl, context->generation,
                               (uvc_host_frame_t *)frame, &msg)) {
        s_frames_dropped++;
        portEXIT_CRITICAL(&s_frame_lock);
        ESP_LOGW(TAG, "no free UVC held-frame slot; returning frame to driver");
        return true;
    }
    portEXIT_CRITICAL(&s_frame_lock);
    if (xQueueSend(s_event_queue, &msg, 0) != pdTRUE) {
        uvc_cancel_held_frame(&msg);
        portENTER_CRITICAL(&s_frame_lock);
        s_frames_dropped++;
        portEXIT_CRITICAL(&s_frame_lock);
        return true;
    }
    UBaseType_t queued = uxQueueMessagesWaiting(s_event_queue);
    portENTER_CRITICAL(&s_frame_lock);
    if (queued > s_frame_queue_high_water) {
        s_frame_queue_high_water = queued;
    }
    portEXIT_CRITICAL(&s_frame_lock);
    return false;
}

static esp_err_t uvc_return_frame(const uvc_app_queue_msg_t *msg)
{
    ESP_RETURN_ON_FALSE(msg != NULL && msg->event == UVC_APP_EVENT_FRAME &&
                        msg->stream_hdl != NULL && msg->frame != NULL &&
                        msg->held_slot < TEST_UVC_FRAME_BUF_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid retained UVC frame message");

    portENTER_CRITICAL(&s_frame_lock);
    uvc_held_frame_slot_t *slot = &s_held_frame_slots[msg->held_slot];
    const bool owned = uvc_held_frame_matches_locked(slot, msg) &&
                       slot->state == UVC_HELD_FRAME_PROCESSING;
    portEXIT_CRITICAL(&s_frame_lock);
    ESP_RETURN_ON_FALSE(owned, ESP_ERR_INVALID_STATE, TAG,
                        "UVC frame slot is not owned by return caller");

    esp_err_t ret = uvc_host_frame_return(msg->stream_hdl, msg->frame);
    portENTER_CRITICAL(&s_frame_lock);
    slot = &s_held_frame_slots[msg->held_slot];
    if (uvc_held_frame_matches_locked(slot, msg) &&
        slot->state == UVC_HELD_FRAME_PROCESSING) {
        if (ret == ESP_OK) {
            *slot = (uvc_held_frame_slot_t) {0};
            if (s_frames_held_by_app > 0) {
                s_frames_held_by_app--;
            }
        } else {
            slot->state = UVC_HELD_FRAME_RETRY;
        }
    }
    portEXIT_CRITICAL(&s_frame_lock);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "uvc_host_frame_return failed (generation=%" PRIu32 "): %s",
                 msg->stream_generation, esp_err_to_name(ret));
    }
    return ret;
}

static void uvc_return_frame_or_retry(const uvc_app_queue_msg_t *msg)
{
    (void)uvc_return_frame(msg);
}

static void uvc_retry_held_frames(void)
{
    for (uint8_t i = 0; i < TEST_UVC_FRAME_BUF_COUNT; ++i) {
        uvc_app_queue_msg_t msg = {0};
        bool retry = false;
        portENTER_CRITICAL(&s_frame_lock);
        uvc_held_frame_slot_t *slot = &s_held_frame_slots[i];
        if (slot->state == UVC_HELD_FRAME_RETRY) {
            slot->state = UVC_HELD_FRAME_PROCESSING;
            msg = slot->msg;
            retry = true;
        }
        portEXIT_CRITICAL(&s_frame_lock);
        if (retry) {
            (void)uvc_return_frame(&msg);
        }
    }
}

static esp_err_t uvc_ensure_rgb565_buf(uint32_t width, uint32_t height)
{
    ESP_RETURN_ON_FALSE(width > 0 && height > 0 && width <= UINT16_MAX && height <= UINT16_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "invalid RGB565 buffer dimensions");
    const size_t aligned_w = ((size_t)width + 15U) & ~15U;
    const size_t aligned_h = ((size_t)height + 15U) & ~15U;
    ESP_RETURN_ON_FALSE(aligned_w <= SIZE_MAX / aligned_h / sizeof(uint16_t),
                        ESP_ERR_INVALID_SIZE, TAG, "RGB565 buffer size overflow");
    const size_t need = aligned_w * aligned_h * sizeof(uint16_t);
    if (s_rgb565_buf != NULL && s_rgb565_buf_size >= need) {
        return ESP_OK;
    }

    const jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t allocated_size = 0;
    uint8_t *new_buf = jpeg_alloc_decoder_mem(need, &mem_cfg, &allocated_size);
    if (new_buf != NULL) {
        uint8_t *old_buf = s_rgb565_buf;
        s_rgb565_buf = new_buf;
        s_rgb565_buf_size = allocated_size;
        free(old_buf);
        ESP_LOGI(TAG, "allocated RGB565 decode buffer: requested=%zu allocated=%zu bytes for %ux%u",
                 need, s_rgb565_buf_size, (unsigned)width, (unsigned)height);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "failed to allocate RGB565 decode buffer: %zu bytes", need);
        return ESP_ERR_NO_MEM;
    }
}

static esp_err_t uvc_close_active_stream(void);
static void uvc_drain_queued_frames(void);

static bool uvc_disconnect_is_pending(uvc_app_queue_msg_t *out_msg)
{
    bool pending;
    portENTER_CRITICAL(&s_frame_lock);
    pending = s_disconnect_pending;
    if (pending && out_msg != NULL) {
        *out_msg = (uvc_app_queue_msg_t) {
            .event = UVC_APP_EVENT_DISCONNECTED,
            .stream_hdl = s_disconnect_stream_hdl,
            .frame = NULL,
            .stream_generation = s_disconnect_stream_generation,
            .held_slot = UVC_HELD_FRAME_SLOT_INVALID,
        };
    }
    portEXIT_CRITICAL(&s_frame_lock);
    return pending;
}

static void uvc_process_disconnect(const uvc_app_queue_msg_t *msg)
{
    if (msg == NULL || msg->event != UVC_APP_EVENT_DISCONNECTED) {
        return;
    }

    /* A lifecycle owner may hold the stream lock while waiting for this worker
     * to stop. Keep the request pending instead of blocking on that lock. */
    uvc_drain_queued_frames();
    uvc_retry_held_frames();
    if (!uvc_stream_lock_take(0)) {
        vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }

    bool matches;
    portENTER_CRITICAL(&s_frame_lock);
    matches = s_disconnect_pending &&
              uvc_active_stream_matches_locked(msg->stream_hdl, msg->stream_generation);
    portEXIT_CRITICAL(&s_frame_lock);
    if (matches) {
        esp_err_t ret = uvc_close_active_stream();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "deferred UVC disconnect cleanup failed: %s", esp_err_to_name(ret));
        } else {
            portENTER_CRITICAL(&s_frame_lock);
            s_disconnect_pending = false;
            s_disconnect_stream_hdl = NULL;
            s_disconnect_stream_generation = 0;
            portEXIT_CRITICAL(&s_frame_lock);
            uvc_set_state(TEST_UVC_STATE_HOST_READY, ESP_OK);
        }
    } else {
        /* The event belongs to an already closed/replaced stream. */
        portENTER_CRITICAL(&s_frame_lock);
        if (s_disconnect_stream_hdl == msg->stream_hdl &&
            s_disconnect_stream_generation == msg->stream_generation) {
            s_disconnect_pending = false;
            s_disconnect_stream_hdl = NULL;
            s_disconnect_stream_generation = 0;
        }
        portEXIT_CRITICAL(&s_frame_lock);
    }
    uvc_stream_lock_give();
}

static void uvc_worker_task(void *arg)
{
    (void)arg;

    uvc_app_queue_msg_t msg;
    while (true) {
        if (s_worker_stop) {
            break;
        }
        uvc_retry_held_frames();
        if (uvc_disconnect_is_pending(&msg)) {
            uvc_process_disconnect(&msg);
            continue;
        }
        if (xQueueReceive(s_event_queue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
            if (s_worker_stop) {
                break;
            }
            portENTER_CRITICAL(&s_frame_lock);
            const bool stream_started = s_stream_started;
            const int64_t last_frame_time_us = s_last_frame_time_us;
            portEXIT_CRITICAL(&s_frame_lock);
            if (stream_started && last_frame_time_us > 0 &&
                esp_timer_get_time() - last_frame_time_us >= TEST_UVC_STREAM_STALL_TIMEOUT_MS * 1000LL) {
                portENTER_CRITICAL(&s_frame_lock);
                s_recovery_required = true;
                portEXIT_CRITICAL(&s_frame_lock);
                uvc_set_state(TEST_UVC_STATE_FAILED, ESP_ERR_TIMEOUT);
            }
            continue;
        }

        if (msg.event == UVC_APP_EVENT_STOP) {
            if (s_worker_stop) {
                break;
            }
            continue;
        }
        if (msg.event == UVC_APP_EVENT_DISCONNECTED) {
            uvc_process_disconnect(&msg);
            continue;
        }
        if (msg.event != UVC_APP_EVENT_FRAME || msg.frame == NULL) {
            continue;
        }
        if (!uvc_claim_held_frame(&msg)) {
            ESP_LOGW(TAG, "discarding UVC queue message without matching held-frame ownership");
            continue;
        }

        uvc_host_frame_t *frame = msg.frame;
        portENTER_CRITICAL(&s_frame_lock);
        const bool stale_frame = s_disconnect_pending ||
                                 !uvc_active_stream_matches_locked(msg.stream_hdl,
                                                                   msg.stream_generation);
        portEXIT_CRITICAL(&s_frame_lock);
        if (stale_frame) {
            uvc_return_frame_or_retry(&msg);
            portENTER_CRITICAL(&s_frame_lock);
            s_frames_dropped++;
            portEXIT_CRITICAL(&s_frame_lock);
            continue;
        }

        uvc_app_queue_msg_t newer;
        while (xQueueReceive(s_event_queue, &newer, 0) == pdTRUE) {
            if (newer.event != UVC_APP_EVENT_FRAME) {
                if (xQueueSendToFront(s_event_queue, &newer, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "failed to restore UVC control event to queue");
                }
                break;
            }
            if (!uvc_claim_held_frame(&newer)) {
                ESP_LOGW(TAG, "discarding coalesced UVC message without held-frame ownership");
                continue;
            }
            uvc_return_frame_or_retry(&msg);
            portENTER_CRITICAL(&s_frame_lock);
            s_frames_dropped++;
            portEXIT_CRITICAL(&s_frame_lock);
            msg = newer;
            frame = newer.frame;
        }
        if (frame == NULL) {
            uvc_return_frame_or_retry(&msg);
            continue;
        }

        portENTER_CRITICAL(&s_frame_lock);
        const bool coalesced_frame_stale = s_disconnect_pending ||
                                           !uvc_active_stream_matches_locked(msg.stream_hdl,
                                                                             msg.stream_generation);
        portEXIT_CRITICAL(&s_frame_lock);
        if (coalesced_frame_stale) {
            uvc_return_frame_or_retry(&msg);
            portENTER_CRITICAL(&s_frame_lock);
            s_frames_dropped++;
            portEXIT_CRITICAL(&s_frame_lock);
            continue;
        }

        portENTER_CRITICAL(&s_frame_lock);
        s_frames_received++;
        s_last_frame_time_us = esp_timer_get_time();
        s_recovery_required = false;
        s_last_frame_bytes = (uint32_t)frame->data_len;
        portEXIT_CRITICAL(&s_frame_lock);

        if (frame->data == NULL || frame->data_len == 0) {
            portENTER_CRITICAL(&s_frame_lock);
            s_decode_errors++;
            const uint32_t decode_errors = s_decode_errors;
            portEXIT_CRITICAL(&s_frame_lock);
            if (uvc_should_sample_error(decode_errors)) {
                ESP_LOGW(TAG, "UVC decode errors=%" PRIu32 ": empty payload", decode_errors);
            }
            uvc_return_frame_or_retry(&msg);
            uvc_log_stats_if_due();
            continue;
        }

        if (s_jpeg_decoder == NULL) {
            portENTER_CRITICAL(&s_frame_lock);
            s_decode_errors++;
            const uint32_t decode_errors = s_decode_errors;
            portEXIT_CRITICAL(&s_frame_lock);
            if (uvc_should_sample_error(decode_errors)) {
                ESP_LOGW(TAG, "UVC decode errors=%" PRIu32 ": JPEG decoder unavailable", decode_errors);
            }
            uvc_return_frame_or_retry(&msg);
            uvc_log_stats_if_due();
            continue;
        }

        jpeg_decode_picture_info_t info = {0};
        esp_err_t ret = jpeg_decoder_get_info(frame->data, (uint32_t)frame->data_len, &info);
        if (ret != ESP_OK) {
            portENTER_CRITICAL(&s_frame_lock);
            s_decode_errors++;
            s_last_error = ret;
            const uint32_t decode_errors = s_decode_errors;
            portEXIT_CRITICAL(&s_frame_lock);
            if (uvc_should_sample_error(decode_errors)) {
                ESP_LOGW(TAG, "UVC decode errors=%" PRIu32 ": get_info=%s len=%u",
                         decode_errors, esp_err_to_name(ret), (unsigned)frame->data_len);
            }
            uvc_return_frame_or_retry(&msg);
            uvc_log_stats_if_due();
            continue;
        }

        portENTER_CRITICAL(&s_frame_lock);
        s_last_frame_w = info.width;
        s_last_frame_h = info.height;
        portEXIT_CRITICAL(&s_frame_lock);
        if (xSemaphoreTake(s_decode_lock, portMAX_DELAY) != pdTRUE) {
            portENTER_CRITICAL(&s_frame_lock);
            s_decode_errors++;
            portEXIT_CRITICAL(&s_frame_lock);
            uvc_return_frame_or_retry(&msg);
            continue;
        }
        ret = uvc_ensure_rgb565_buf(info.width, info.height);
        if (ret != ESP_OK) {
            portENTER_CRITICAL(&s_frame_lock);
            s_decode_errors++;
            s_last_error = ret;
            portEXIT_CRITICAL(&s_frame_lock);
            uvc_return_frame_or_retry(&msg);
            xSemaphoreGive(s_decode_lock);
            uvc_log_stats_if_due();
            continue;
        }

        const jpeg_decode_cfg_t decode_cfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
            .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
        };

        int64_t start_us = esp_timer_get_time();
        uint32_t out_size = 0;
        ret = jpeg_decoder_process(s_jpeg_decoder, &decode_cfg,
                                   frame->data, (uint32_t)frame->data_len,
                                   s_rgb565_buf, (uint32_t)s_rgb565_buf_size, &out_size);
        portENTER_CRITICAL(&s_frame_lock);
        s_last_decode_us = (uint32_t)(esp_timer_get_time() - start_us);

        if (ret != ESP_OK) {
            s_decode_errors++;
            s_last_error = ret;
            const uint32_t decode_errors = s_decode_errors;
            portEXIT_CRITICAL(&s_frame_lock);
            if (uvc_should_sample_error(decode_errors)) {
                ESP_LOGW(TAG, "UVC decode errors=%" PRIu32 ": process=%s len=%u %ux%u",
                         decode_errors, esp_err_to_name(ret), (unsigned)frame->data_len,
                         (unsigned)info.width, (unsigned)info.height);
            }
        } else {
            s_frames_decoded++;
            portEXIT_CRITICAL(&s_frame_lock);
        }

        uvc_return_frame_or_retry(&msg);
        if (ret == ESP_OK) {
            int64_t now_us = esp_timer_get_time();
            if (now_us - s_preview_last_update_us >= TEST_UVC_PREVIEW_UPDATE_INTERVAL_MS * 1000LL) {
                s_preview_last_update_us = now_us;
                uvc_update_preview(info.width, info.height, out_size);
            }
        }
        xSemaphoreGive(s_decode_lock);
        uvc_log_stats_if_due();
    }

    if (s_device_event_group != NULL) {
        xEventGroupSetBits(s_device_event_group, UVC_WORKER_EXITED_BIT);
    }
    portENTER_CRITICAL(&s_frame_lock);
    s_worker_task_handle = NULL;
    portEXIT_CRITICAL(&s_frame_lock);
    vTaskDelete(NULL);
}

static void uvc_drain_queued_frames(void)
{
    uvc_app_queue_msg_t msg;
    while (s_event_queue != NULL && xQueueReceive(s_event_queue, &msg, 0) == pdTRUE) {
        if (msg.event == UVC_APP_EVENT_FRAME) {
            if (uvc_claim_held_frame(&msg)) {
                uvc_return_frame_or_retry(&msg);
            } else {
                ESP_LOGW(TAG, "discarding drained UVC message without held-frame ownership");
            }
        } else if (msg.event == UVC_APP_EVENT_DISCONNECTED) {
            portENTER_CRITICAL(&s_frame_lock);
            if (uvc_active_stream_matches_locked(msg.stream_hdl,
                                                 msg.stream_generation)) {
                s_disconnect_pending = true;
                s_disconnect_stream_hdl = msg.stream_hdl;
                s_disconnect_stream_generation = msg.stream_generation;
            }
            portEXIT_CRITICAL(&s_frame_lock);
        } else if (msg.event == UVC_APP_EVENT_STOP) {
            (void)xQueueSendToFront(s_event_queue, &msg, 0);
            break;
        }
    }
}

static esp_err_t uvc_wait_for_app_frames(void)
{
    for (uint32_t i = 0; i < 50; ++i) {
        uvc_drain_queued_frames();
        uvc_retry_held_frames();
        portENTER_CRITICAL(&s_frame_lock);
        uint32_t held = s_frames_held_by_app;
        portEXIT_CRITICAL(&s_frame_lock);
        if (held == 0) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_ERR_TIMEOUT;
}

static void uvc_usblib_task(void *arg)
{
    (void)arg;

    while (!s_usblib_stop) {
        uint32_t event_flags = 0;
        esp_err_t ret = usb_host_lib_handle_events(pdMS_TO_TICKS(100), &event_flags);
        if (ret == ESP_ERR_NOT_FINISHED) {
            if (s_usblib_stop) {
                break;
            }
            continue;
        }
        /* ESP_ERR_TIMEOUT 表示无事件（空闲），不是错误；过滤掉避免刷屏。
         * 只对其它非 OK 返回告警。 */
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            s_usblib_errors++;
            if (uvc_should_sample_error(s_usblib_errors)) {
                ESP_LOGW(TAG, "USB host lib errors=%" PRIu32 ", latest=%s",
                         s_usblib_errors, esp_err_to_name(ret));
            }
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            esp_err_t free_ret = usb_host_device_free_all();
            if (free_ret == ESP_OK) {
                ESP_LOGI(TAG, "USB Host: no clients or allocated devices");
                xEventGroupSetBits(s_device_event_group, UVC_USB_ALL_FREE_BIT);
            } else if (free_ret != ESP_ERR_NOT_FINISHED) {
                ESP_LOGW(TAG, "usb_host_device_free_all failed: %s", esp_err_to_name(free_ret));
            }
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB Host: all devices freed");
            xEventGroupSetBits(s_device_event_group, UVC_USB_ALL_FREE_BIT);
        }
    }

    if (s_device_event_group != NULL) {
        xEventGroupSetBits(s_device_event_group, UVC_USBLIB_EXITED_BIT);
    }
    portENTER_CRITICAL(&s_frame_lock);
    s_usblib_task_handle = NULL;
    portEXIT_CRITICAL(&s_frame_lock);
    vTaskDelete(NULL);
}

static void uvc_free_app_resources(void)
{
    if (s_rgb565_buf != NULL) {
        free(s_rgb565_buf);
        s_rgb565_buf = NULL;
        s_rgb565_buf_size = 0;
    }

    if (s_preview_lock != NULL && xSemaphoreTake(s_preview_lock, portMAX_DELAY) == pdTRUE) {
        if (s_preview_buf != NULL) {
            heap_caps_free(s_preview_buf);
            s_preview_buf = NULL;
        }
        s_preview_frame_id = 0;
        xSemaphoreGive(s_preview_lock);
    }

    if (s_event_queue != NULL) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
    }

    if (s_device_event_group != NULL) {
        vEventGroupDelete(s_device_event_group);
        s_device_event_group = NULL;
    }
}

static esp_err_t uvc_stop_worker_task(void)
{
    if (!uvc_task_running(&s_worker_task_handle)) {
        return ESP_OK;
    }
    s_worker_stop = true;
    const uvc_app_queue_msg_t stop = {
        .event = UVC_APP_EVENT_STOP,
        .stream_hdl = NULL,
        .frame = NULL,
        .stream_generation = 0,
        .held_slot = UVC_HELD_FRAME_SLOT_INVALID,
    };
    (void)xQueueSendToFront(s_event_queue, &stop, 0);
    EventBits_t bits = xEventGroupWaitBits(s_device_event_group, UVC_WORKER_EXITED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(UVC_TASK_STOP_TIMEOUT_MS));
    if ((bits & UVC_WORKER_EXITED_BIT) == 0) {
        return ESP_ERR_TIMEOUT;
    }
    return uvc_wait_task_stopped(&s_worker_task_handle);
}

static esp_err_t uvc_stop_usblib_task(void)
{
    if (!uvc_task_running(&s_usblib_task_handle)) {
        return ESP_OK;
    }
    s_usblib_stop = true;
    esp_err_t ret = usb_host_lib_unblock();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    EventBits_t bits = xEventGroupWaitBits(s_device_event_group, UVC_USBLIB_EXITED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(UVC_TASK_STOP_TIMEOUT_MS));
    if ((bits & UVC_USBLIB_EXITED_BIT) == 0) {
        return ESP_ERR_TIMEOUT;
    }
    return uvc_wait_task_stopped(&s_usblib_task_handle);
}

static esp_err_t uvc_init_locked(void)
{
    esp_err_t ret = ESP_OK;

    if (s_initialized) {
        return s_module.state == TEST_MODULE_STATE_ERROR ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    ESP_RETURN_ON_FALSE(!s_usb_host_installed && !s_uvc_driver_installed &&
                        s_worker_task_handle == NULL && s_usblib_task_handle == NULL &&
                        s_event_queue == NULL && s_device_event_group == NULL &&
                        s_stream_context == NULL,
                        ESP_ERR_INVALID_STATE, TAG, "previous UVC teardown incomplete");

    uvc_reset_stream_context_pool();
    uvc_reset_held_frames();

    (void)test_uvc_should_resume_after_reset();

    ESP_LOGI(TAG, "Initializing USB UVC camera test module on High-Speed USB OTG / UTMI PHY (peripheral_map=0x%x)",
             (unsigned)TEST_UVC_USB_HOST_PERIPHERAL_MAP);
    portENTER_CRITICAL(&s_frame_lock);
    s_stopping = false;
    s_worker_stop = false;
    s_usblib_stop = false;
    s_waiting_for_all_free = false;
    s_controller_released = true;
    s_initialized = true;
    portEXIT_CRITICAL(&s_frame_lock);
    uvc_set_module_state(TEST_MODULE_STATE_INITIALIZING, ESP_OK, false);

    s_event_queue = xQueueCreate(TEST_UVC_FRAME_QUEUE_LEN, sizeof(uvc_app_queue_msg_t));
    ESP_GOTO_ON_FALSE(s_event_queue != NULL, ESP_ERR_NO_MEM, fail,
                      TAG, "create UVC frame queue failed");

    s_device_event_group = xEventGroupCreate();
    ESP_GOTO_ON_FALSE(s_device_event_group != NULL, ESP_ERR_NO_MEM, fail,
                      TAG, "create UVC device event group failed");

    if (s_preview_lock == NULL) {
        s_preview_lock = xSemaphoreCreateMutex();
    }
    ESP_GOTO_ON_FALSE(s_preview_lock != NULL, ESP_ERR_NO_MEM, fail,
                      TAG, "create permanent UVC preview lock failed");
    if (s_decode_lock == NULL) {
        s_decode_lock = xSemaphoreCreateMutex();
    }
    ESP_GOTO_ON_FALSE(s_decode_lock != NULL, ESP_ERR_NO_MEM, fail,
                      TAG, "create permanent UVC decode lock failed");
    s_preview_buf = heap_caps_aligned_calloc(TEST_UVC_PPA_BUFFER_ALIGNMENT,
                                             TEST_UVC_PREVIEW_H_RES * TEST_UVC_PREVIEW_V_RES,
                                             sizeof(uint16_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_GOTO_ON_FALSE(s_preview_buf != NULL, ESP_ERR_NO_MEM, fail,
                      TAG, "allocate UVC preview buffer failed");
    s_preview_frame_id = 0;
    s_preview_source_w = 0;
    s_preview_source_h = 0;
    s_preview_dst_w = 0;
    s_preview_dst_h = 0;
    s_preview_dst_x = 0;
    s_preview_dst_y = 0;
    s_preview_last_update_us = 0;
    const ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ret = ppa_register_client(&ppa_config, &s_ppa_srm);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PPA preview scaling unavailable, use CPU fallback: %s", esp_err_to_name(ret));
        s_ppa_srm = NULL;
        ret = ESP_OK;
    }
    portENTER_CRITICAL(&s_frame_lock);
    s_frame_info_count = 0;
    s_accept_frames = false;
    s_disconnect_pending = false;
    s_disconnect_stream_hdl = NULL;
    s_disconnect_stream_generation = 0;
    s_stream_hdl = NULL;
    s_stream_opened = false;
    s_stream_started = false;
    s_device_connected = false;
    s_format_negotiated = false;
    s_frames_received = 0;
    s_frames_decoded = 0;
    s_decode_errors = 0;
    s_frames_dropped = 0;
    s_frame_queue_high_water = 0;
    s_last_frame_bytes = 0;
    s_last_decode_us = 0;
    s_last_preview_us = 0;
    s_last_frame_w = 0;
    s_last_frame_h = 0;
    s_last_frame_time_us = 0;
    s_recovery_level = TEST_UVC_RECOVERY_NONE;
    s_recovery_attempts = 0;
    s_recovery_required = false;
    s_preview_backend = s_ppa_srm != NULL ? TEST_UVC_PREVIEW_BACKEND_PPA : TEST_UVC_PREVIEW_BACKEND_CPU;
    s_ppa_runtime_disabled = false;
    s_preview_fallbacks = 0;
    s_transfer_errors = 0;
    s_frame_overflows = 0;
    s_frame_underflows = 0;
    s_usblib_errors = 0;
    s_last_error = ESP_OK;
    portEXIT_CRITICAL(&s_frame_lock);
    s_stats_window_start_us = 0;
    s_stats_window_received = 0;
    s_stats_window_decoded = 0;
    s_stats_window_errors = 0;

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .root_port_unpowered = true,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        .peripheral_map = TEST_UVC_USB_HOST_PERIPHERAL_MAP,
    };
    ESP_GOTO_ON_ERROR(usb_host_install(&host_config), fail, TAG, "usb_host_install failed");
    s_usb_host_installed = true;
    portENTER_CRITICAL(&s_frame_lock);
    s_controller_released = false;
    portEXIT_CRITICAL(&s_frame_lock);

    BaseType_t task_created = xTaskCreatePinnedToCore(uvc_usblib_task,
                                                      "uvc_usblib",
                                                      TEST_UVC_USB_LIB_TASK_STACK,
                                                      NULL,
                                                      TEST_UVC_USB_LIB_TASK_PRIORITY,
                                                      &s_usblib_task_handle,
                                                      TEST_UVC_USB_LIB_TASK_CORE);
    ESP_GOTO_ON_FALSE(task_created == pdTRUE, ESP_ERR_NO_MEM, fail, TAG, "create USB lib task failed");

    const uvc_host_driver_config_t uvc_driver_config = {
        .driver_task_stack_size = TEST_UVC_DRIVER_TASK_STACK,
        .driver_task_priority = TEST_UVC_DRIVER_TASK_PRIORITY,
        .xCoreID = TEST_UVC_DRIVER_TASK_CORE,
        .create_background_task = true,
        .event_cb = uvc_driver_event_callback,
        .user_ctx = NULL,
    };
    ESP_GOTO_ON_ERROR(uvc_host_install(&uvc_driver_config), fail, TAG, "uvc_host_install failed");
    s_uvc_driver_installed = true;

    const jpeg_decode_engine_cfg_t jpeg_dec_cfg = {
        .intr_priority = 0,
        .timeout_ms = TEST_UVC_JPEG_DECODE_TIMEOUT_MS,
        .flags = { .allow_pd = 0 },
    };
    uvc_log_memory("before JPEG decoder");
    ESP_GOTO_ON_ERROR(jpeg_new_decoder_engine(&jpeg_dec_cfg, &s_jpeg_decoder),
                      fail, TAG, "jpeg_new_decoder_engine failed");
    uvc_log_memory("after JPEG decoder");

    task_created = xTaskCreatePinnedToCore(uvc_worker_task,
                                          "uvc_worker",
                                          TEST_UVC_WORKER_TASK_STACK,
                                          NULL,
                                          TEST_UVC_WORKER_TASK_PRIORITY,
                                          &s_worker_task_handle,
                                          TEST_UVC_WORKER_TASK_CORE);
    ESP_GOTO_ON_FALSE(task_created == pdTRUE, ESP_ERR_NO_MEM, fail, TAG, "create UVC worker task failed");

    s_root_port_powered = false;
    uvc_set_state(TEST_UVC_STATE_HOST_READY, ESP_OK);
    uvc_set_module_state(TEST_MODULE_STATE_READY, ESP_OK, true);
    ESP_LOGI(TAG, "UVC camera test module initialized, waiting for UVC device (vid=0x%x pid=0x%x prefer %ux%u MJPEG)",
             (unsigned)TEST_UVC_VID, (unsigned)TEST_UVC_PID,
             (unsigned)TEST_UVC_PREFER_H_RES, (unsigned)TEST_UVC_PREFER_V_RES);
    return ESP_OK;

fail: {
    const esp_err_t init_error = ret;
    esp_err_t cleanup_error = test_uvc_deinit();
    if (cleanup_error != ESP_OK) {
        ESP_LOGE(TAG, "UVC init rollback incomplete: %s", esp_err_to_name(cleanup_error));
    }
    uvc_set_module_state(TEST_MODULE_STATE_ERROR, init_error, false);
    return init_error;
}
}

esp_err_t test_uvc_init(void)
{
    ESP_RETURN_ON_ERROR(uvc_ensure_stream_lock(), TAG, "create UVC stream lock failed");
    ESP_RETURN_ON_FALSE(uvc_stream_lock_take(portMAX_DELAY), ESP_ERR_INVALID_STATE, TAG,
                        "UVC stream lock unavailable");
    esp_err_t ret = uvc_init_locked();
    uvc_stream_lock_give();
    return ret;
}

static esp_err_t uvc_run_locked(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "UVC not initialized, call test_uvc_init first");

    portENTER_CRITICAL(&s_frame_lock);
    const bool stream_opened = s_stream_opened;
    const bool stream_started = s_stream_started;
    const bool disconnect_pending = s_disconnect_pending;
    uvc_host_stream_hdl_t current_stream_hdl = s_stream_hdl;
    uvc_stream_context_t *current_context = s_stream_context;
    portEXIT_CRITICAL(&s_frame_lock);
    if (stream_opened) {
        if (stream_started) {
            return ESP_OK;
        }
        ESP_RETURN_ON_FALSE(!disconnect_pending && current_context != NULL &&
                            current_context->state == UVC_STREAM_CONTEXT_ACTIVE &&
                            current_context->stream_hdl == current_stream_hdl,
                            ESP_ERR_INVALID_STATE, TAG,
                            "UVC open stream has no active callback context");
        esp_err_t start_ret = uvc_host_stream_start(current_stream_hdl);
        if (start_ret != ESP_OK) {
            portENTER_CRITICAL(&s_frame_lock);
            s_recovery_required = true;
            portEXIT_CRITICAL(&s_frame_lock);
            uvc_set_state(TEST_UVC_STATE_FAILED, start_ret);
            return start_ret;
        }
        portENTER_CRITICAL(&s_frame_lock);
        s_stream_started = true;
        s_accept_frames = true;
        s_last_frame_time_us = esp_timer_get_time();
        s_recovery_required = false;
        portEXIT_CRITICAL(&s_frame_lock);
        s_uvc_streaming_marker = UVC_STREAMING_MARKER;
        uvc_set_state(TEST_UVC_STATE_STREAMING, ESP_OK);
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_frame_lock);
    const bool stream_owner_clear = s_stream_context == NULL && s_stream_hdl == NULL;
    portEXIT_CRITICAL(&s_frame_lock);
    ESP_RETURN_ON_FALSE(stream_owner_clear, ESP_ERR_INVALID_STATE, TAG,
                        "UVC stream ownership is inconsistent before open");

    uvc_set_state(TEST_UVC_STATE_ENUMERATING, ESP_OK);
    if (!s_root_port_powered) {
        vTaskDelay(pdMS_TO_TICKS(TEST_UVC_ROOT_PORT_CYCLE_DELAY_MS));
        esp_err_t power_ret = usb_host_lib_set_root_port_power(true);
        if (power_ret != ESP_OK) {
            uvc_set_state(TEST_UVC_STATE_FAILED, power_ret);
            return power_ret;
        }
        portENTER_CRITICAL(&s_frame_lock);
        s_root_port_powered = true;
        portEXIT_CRITICAL(&s_frame_lock);
    }

    bool connected = false;
    for (uint32_t attempt = 1; attempt <= TEST_UVC_ENUM_MAX_ATTEMPTS; ++attempt) {
        EventBits_t bits = xEventGroupWaitBits(s_device_event_group,
                                               UVC_DEVICE_CONNECTED_BIT,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(TEST_UVC_ENUM_TIMEOUT_MS));
        if ((bits & UVC_DEVICE_CONNECTED_BIT) != 0) {
            connected = true;
            break;
        }

        if (attempt == TEST_UVC_ENUM_MAX_ATTEMPTS) {
            break;
        }

        ESP_LOGW(TAG, "UVC device not enumerated (attempt %u/%u), cycling root port",
                 (unsigned)attempt, (unsigned)TEST_UVC_ENUM_MAX_ATTEMPTS);
        ESP_RETURN_ON_ERROR(usb_host_lib_set_root_port_power(false), TAG,
                            "stop UVC root port failed");
        portENTER_CRITICAL(&s_frame_lock);
        s_root_port_powered = false;
        portEXIT_CRITICAL(&s_frame_lock);
        vTaskDelay(pdMS_TO_TICKS(TEST_UVC_ROOT_PORT_CYCLE_DELAY_MS));
        portENTER_CRITICAL(&s_frame_lock);
        s_device_connected = false;
        s_format_negotiated = false;
        portEXIT_CRITICAL(&s_frame_lock);
        xEventGroupClearBits(s_device_event_group, UVC_DEVICE_CONNECTED_BIT);
        ESP_RETURN_ON_ERROR(usb_host_lib_set_root_port_power(true), TAG,
                            "restart UVC root port failed");
        portENTER_CRITICAL(&s_frame_lock);
        s_root_port_powered = true;
        portEXIT_CRITICAL(&s_frame_lock);
    }

    if (!connected) {
        ESP_LOGE(TAG, "UVC device did not enumerate after %u attempts",
                 (unsigned)TEST_UVC_ENUM_MAX_ATTEMPTS);
        uvc_set_state(TEST_UVC_STATE_FAILED, ESP_ERR_NOT_FOUND);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "waiting %u ms for UVC device control endpoint to stabilize",
             (unsigned)TEST_UVC_DEVICE_READY_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(TEST_UVC_DEVICE_READY_DELAY_MS));

    portENTER_CRITICAL(&s_frame_lock);
    const bool format_negotiated = s_format_negotiated;
    const uvc_host_stream_format_t negotiated_format = s_negotiated_format;
    portEXIT_CRITICAL(&s_frame_lock);
    const uint32_t decode_h_res = format_negotiated ? negotiated_format.h_res
                                                     : TEST_UVC_PREFER_H_RES;
    const uint32_t decode_v_res = format_negotiated ? negotiated_format.v_res
                                                     : TEST_UVC_PREFER_V_RES;
    ESP_RETURN_ON_FALSE(s_decode_lock != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "UVC decode lock is unavailable");
    xSemaphoreTake(s_decode_lock, portMAX_DELAY);
    esp_err_t ret = uvc_ensure_rgb565_buf(decode_h_res, decode_v_res);
    xSemaphoreGive(s_decode_lock);
    if (ret != ESP_OK) {
        uvc_set_state(TEST_UVC_STATE_FAILED, ret);
        return ret;
    }

    uvc_stream_context_t *stream_context = uvc_reserve_stream_context();
    if (stream_context == NULL) {
        portENTER_CRITICAL(&s_frame_lock);
        s_recovery_required = true;
        portEXIT_CRITICAL(&s_frame_lock);
        ESP_LOGE(TAG, "UVC stream context pool exhausted; stack recovery is required");
        uvc_set_state(TEST_UVC_STATE_FAILED, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    const uvc_host_stream_config_t stream_config = {
        .event_cb = uvc_stream_event_callback,
        .frame_cb = uvc_frame_callback,
        .user_ctx = stream_context,
        .usb = {
            .dev_addr = UVC_HOST_ANY_DEV_ADDR,
            .vid = TEST_UVC_VID,
            .pid = TEST_UVC_PID,
            .uvc_stream_index = TEST_UVC_UVC_STREAM_INDEX,
        },
        .vs_format = format_negotiated ? negotiated_format
                                          : (uvc_host_stream_format_t) {
                                                .h_res = TEST_UVC_PREFER_H_RES,
                                                .v_res = TEST_UVC_PREFER_V_RES,
                                                .fps = 0,
                                                .format = TEST_UVC_FORMAT,
                                            },
        .advanced = {
            .number_of_frame_buffers = TEST_UVC_FRAME_BUF_COUNT,
            .frame_size = TEST_UVC_FRAME_SIZE,
            .frame_heap_caps = TEST_UVC_FRAME_HEAP_CAPS,
            .number_of_urbs = TEST_UVC_NUMBER_OF_URBS,
            .urb_size = TEST_UVC_URB_SIZE,
            .user_frame_buffers = NULL,
        },
    };

    ESP_LOGI(TAG, "Opening UVC stream %ux%u@%.1fFPS format=%d on High-Speed USB OTG...",
             (unsigned)stream_config.vs_format.h_res,
             (unsigned)stream_config.vs_format.v_res,
             stream_config.vs_format.fps,
             (int)stream_config.vs_format.format);

    ret = ESP_FAIL;
    uvc_host_stream_hdl_t new_stream_hdl = NULL;
    for (uint32_t attempt = 1; attempt <= TEST_UVC_STREAM_OPEN_MAX_ATTEMPTS; ++attempt) {
        ret = uvc_host_stream_open(&stream_config, 0, &new_stream_hdl);
        if (ret == ESP_OK) {
            break;
        }

        new_stream_hdl = NULL;
        if (ret != ESP_ERR_INVALID_RESPONSE || attempt == TEST_UVC_STREAM_OPEN_MAX_ATTEMPTS) {
            uvc_release_reserved_stream_context(stream_context);
            portENTER_CRITICAL(&s_frame_lock);
            s_last_error = ret;
            portEXIT_CRITICAL(&s_frame_lock);
            ESP_LOGE(TAG, "uvc_host_stream_open failed: %s", esp_err_to_name(ret));
            uvc_set_state(TEST_UVC_STATE_FAILED, ret);
            return ret;
        }

        ESP_LOGW(TAG, "UVC stream negotiation failed (attempt %u/%u), retrying",
                 (unsigned)attempt, (unsigned)TEST_UVC_STREAM_OPEN_MAX_ATTEMPTS);
        vTaskDelay(pdMS_TO_TICKS(TEST_UVC_STREAM_OPEN_RETRY_DELAY_MS));
    }

    /* The lifecycle lock excludes every other context publisher/retirer from
     * the pre-open ownership check through this atomic publication. */
    uvc_publish_stream_context(stream_context, new_stream_hdl);
    portENTER_CRITICAL(&s_frame_lock);
    s_stream_hdl = new_stream_hdl;
    s_disconnect_pending = false;
    s_disconnect_stream_hdl = NULL;
    s_disconnect_stream_generation = 0;
    s_stream_opened = true;
    portEXIT_CRITICAL(&s_frame_lock);
    ret = uvc_host_stream_start(new_stream_hdl);
    if (ret != ESP_OK) {
        const esp_err_t start_error = ret;
        portENTER_CRITICAL(&s_frame_lock);
        s_last_error = start_error;
        s_recovery_required = true;
        portEXIT_CRITICAL(&s_frame_lock);
        esp_err_t close_ret = uvc_host_stream_close(new_stream_hdl);
        if (close_ret == ESP_OK) {
            uvc_retire_stream_context(stream_context);
            portENTER_CRITICAL(&s_frame_lock);
            s_stream_hdl = NULL;
            s_stream_opened = false;
            portEXIT_CRITICAL(&s_frame_lock);
        } else {
            ESP_LOGE(TAG, "close UVC stream after start failure failed; handle retained: %s",
                     esp_err_to_name(close_ret));
        }
        uvc_set_state(TEST_UVC_STATE_FAILED, start_error);
        return start_error;
    }
    portENTER_CRITICAL(&s_frame_lock);
    s_stream_started = true;
    s_accept_frames = true;
    s_uvc_streaming_marker = UVC_STREAMING_MARKER;
    s_last_frame_time_us = esp_timer_get_time();
    s_recovery_required = false;
    s_last_error = ESP_OK;
    portEXIT_CRITICAL(&s_frame_lock);
    uvc_set_state(TEST_UVC_STATE_STREAMING, ESP_OK);
    ESP_LOGI(TAG, "UVC stream started");
    return ESP_OK;
}

esp_err_t test_uvc_run(void)
{
    ESP_RETURN_ON_ERROR(uvc_ensure_stream_lock(), TAG, "create UVC stream lock failed");
    ESP_RETURN_ON_FALSE(uvc_stream_lock_take(portMAX_DELAY), ESP_ERR_INVALID_STATE, TAG,
                        "UVC stream lock unavailable");
    esp_err_t ret = uvc_run_locked();
    uvc_stream_lock_give();
    return ret;
}

esp_err_t test_uvc_get_status(test_uvc_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != NULL, ESP_ERR_INVALID_ARG, TAG, "out_status is NULL");

    portENTER_CRITICAL(&s_frame_lock);
    *out_status = (test_uvc_status_t) {
        .module = s_module,
        .state = s_state,
        .initialized = s_initialized,
        .controller_released = s_controller_released,
        .stream_opened = s_stream_opened,
        .stream_started = s_stream_started,
        .device_connected = s_device_connected,
        .format_negotiated = s_format_negotiated,
        .negotiated_h_res = s_format_negotiated ? s_negotiated_format.h_res : 0,
        .negotiated_v_res = s_format_negotiated ? s_negotiated_format.v_res : 0,
        .negotiated_fps = s_format_negotiated ? s_negotiated_format.fps : 0.0f,
        .frames_received = s_frames_received,
        .frames_decoded = s_frames_decoded,
        .decode_errors = s_decode_errors,
        .frames_dropped = s_frames_dropped,
        .frame_queue_high_water = s_frame_queue_high_water,
        .last_frame_bytes = s_last_frame_bytes,
        .last_decode_us = s_last_decode_us,
        .last_preview_us = s_last_preview_us,
        .last_frame_w = s_last_frame_w,
        .last_frame_h = s_last_frame_h,
        .last_frame_time_us = s_last_frame_time_us,
        .recovery_level = s_recovery_level,
        .recovery_attempts = s_recovery_attempts,
        .recovery_required = s_recovery_required,
        .warm_recovery_expected = s_uvc_streaming_marker == UVC_STREAMING_MARKER,
        .preview_backend = s_preview_backend,
        .preview_fallbacks = s_preview_fallbacks,
        .transfer_errors = s_transfer_errors,
        .frame_overflows = s_frame_overflows,
        .frame_underflows = s_frame_underflows,
        .last_error = s_last_error,
    };
    portEXIT_CRITICAL(&s_frame_lock);
    return ESP_OK;
}

esp_err_t test_uvc_get_resolutions(test_uvc_resolution_t *out_list, size_t *inout_count)
{
    ESP_RETURN_ON_FALSE(inout_count != NULL, ESP_ERR_INVALID_ARG, TAG, "inout_count is NULL");

    uvc_host_frame_info_t frame_info_list[TEST_UVC_FRAME_INFO_MAX];
    portENTER_CRITICAL(&s_frame_lock);
    const size_t frame_info_count = s_frame_info_count;
    memcpy(frame_info_list, s_frame_info_list, sizeof(frame_info_list));
    portEXIT_CRITICAL(&s_frame_lock);

    size_t total = 0;
    for (size_t i = 0; i < frame_info_count; ++i) {
        const uvc_host_frame_info_t *info = &frame_info_list[i];
        if (info->format != TEST_UVC_FORMAT) {
            continue;
        }
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j) {
            if (frame_info_list[j].format == TEST_UVC_FORMAT &&
                frame_info_list[j].h_res == info->h_res &&
                frame_info_list[j].v_res == info->v_res) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        if (out_list != NULL && total < *inout_count) {
            out_list[total] = (test_uvc_resolution_t) {
                .h_res = info->h_res,
                .v_res = info->v_res,
                .fps = uvc_interval_to_fps(uvc_pick_best_interval(info)),
            };
        }
        total++;
    }
    if (out_list != NULL && *inout_count < total) {
        *inout_count = total;
        return ESP_ERR_INVALID_SIZE;
    }
    *inout_count = total;
    return total == 0 ? ESP_ERR_NOT_FOUND : ESP_OK;
}

esp_err_t test_uvc_set_preferred_resolution(uint32_t h_res, uint32_t v_res)
{
    ESP_RETURN_ON_FALSE(h_res > 0 && v_res > 0, ESP_ERR_INVALID_ARG, TAG, "invalid UVC resolution");
    portENTER_CRITICAL(&s_frame_lock);
    s_requested_h_res = h_res;
    s_requested_v_res = v_res;
    s_requested_resolution_valid = true;
    portEXIT_CRITICAL(&s_frame_lock);
    return ESP_OK;
}

static esp_err_t uvc_change_resolution_locked(uint32_t h_res, uint32_t v_res)
{
    ESP_RETURN_ON_ERROR(test_uvc_set_preferred_resolution(h_res, v_res), TAG,
                        "set UVC preferred resolution failed");
    uvc_host_frame_info_t selected_info = {0};
    bool selected_valid = false;
    uint32_t selected_interval = 0;
    portENTER_CRITICAL(&s_frame_lock);
    const bool stream_available = s_initialized && s_stream_context != NULL &&
                                  s_stream_context->state == UVC_STREAM_CONTEXT_ACTIVE &&
                                  s_stream_context->stream_hdl != NULL && s_stream_opened;
    for (size_t i = 0; stream_available && i < s_frame_info_count; ++i) {
        const uvc_host_frame_info_t *info = &s_frame_info_list[i];
        if (info->format != TEST_UVC_FORMAT || info->h_res != h_res || info->v_res != v_res) {
            continue;
        }
        uint32_t interval = uvc_pick_best_interval(info);
        if (!selected_valid || interval < selected_interval) {
            selected_info = *info;
            selected_valid = true;
            selected_interval = interval;
        }
    }
    portEXIT_CRITICAL(&s_frame_lock);
    ESP_RETURN_ON_FALSE(stream_available && selected_valid, stream_available ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE,
                        TAG,
                        "requested UVC resolution %ux%u is unavailable",
                        (unsigned)h_res, (unsigned)v_res);

    uvc_host_frame_info_t *selected = &selected_info;
    portENTER_CRITICAL(&s_frame_lock);
    s_accept_frames = false;
    portEXIT_CRITICAL(&s_frame_lock);
    esp_err_t ret = ESP_OK;
    portENTER_CRITICAL(&s_frame_lock);
    const bool was_stream_started = s_stream_started;
    uvc_host_stream_hdl_t stream_hdl = s_stream_hdl;
    portEXIT_CRITICAL(&s_frame_lock);
    if (was_stream_started) {
        ret = uvc_host_stream_stop(stream_hdl);
        if (ret != ESP_OK) {
            goto recovery_required;
        }
        portENTER_CRITICAL(&s_frame_lock);
        s_stream_started = false;
        portEXIT_CRITICAL(&s_frame_lock);
    }
    ret = uvc_wait_for_app_frames();
    if (ret != ESP_OK) {
        goto recovery_required;
    }

    if (xSemaphoreTake(s_decode_lock, portMAX_DELAY) != pdTRUE) {
        ret = ESP_ERR_TIMEOUT;
        goto recovery_required;
    }
    ret = uvc_ensure_rgb565_buf(selected->h_res, selected->v_res);
    xSemaphoreGive(s_decode_lock);
    if (ret != ESP_OK) {
        goto recovery_required;
    }

    uvc_host_stream_format_t format = {
        .h_res = selected->h_res,
        .v_res = selected->v_res,
        .fps = uvc_interval_to_fps(selected_interval),
        .format = selected->format,
    };
    ret = uvc_host_stream_format_select(stream_hdl, &format);
    if (ret != ESP_OK) {
        goto recovery_required;
    }
    ret = uvc_host_stream_start(stream_hdl);
    if (ret != ESP_OK) {
        goto recovery_required;
    }

    portENTER_CRITICAL(&s_frame_lock);
    s_negotiated_format = format;
    s_format_negotiated = true;
    s_stream_started = true;
    s_accept_frames = true;
    s_uvc_streaming_marker = UVC_STREAMING_MARKER;
    s_last_frame_time_us = esp_timer_get_time();
    s_recovery_required = false;
    s_last_error = ESP_OK;
    portEXIT_CRITICAL(&s_frame_lock);
    uvc_set_state(TEST_UVC_STATE_STREAMING, ESP_OK);
    ESP_LOGI(TAG, "UVC stream changed to %ux%u@%.1fFPS", (unsigned)format.h_res,
             (unsigned)format.v_res, format.fps);
    return ESP_OK;

recovery_required:
    portENTER_CRITICAL(&s_frame_lock);
    s_last_error = ret;
    s_recovery_required = true;
    portEXIT_CRITICAL(&s_frame_lock);
    uvc_set_state(TEST_UVC_STATE_FAILED, ret);
    ESP_LOGE(TAG, "change UVC resolution failed, recovery required: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t test_uvc_change_resolution(uint32_t h_res, uint32_t v_res)
{
    ESP_RETURN_ON_ERROR(uvc_ensure_stream_lock(), TAG, "create UVC stream lock failed");
    ESP_RETURN_ON_FALSE(uvc_stream_lock_take(portMAX_DELAY), ESP_ERR_INVALID_STATE, TAG,
                        "UVC stream lock unavailable");
    esp_err_t ret = uvc_change_resolution_locked(h_res, v_res);
    uvc_stream_lock_give();
    return ret;
}

static esp_err_t uvc_close_active_stream(void)
{
    portENTER_CRITICAL(&s_frame_lock);
    s_accept_frames = false;
    uvc_stream_context_t *stream_context = s_stream_context;
    uvc_host_stream_hdl_t stream_hdl = stream_context != NULL ? stream_context->stream_hdl : NULL;
    const uint32_t stream_generation = stream_context != NULL ? stream_context->generation : 0;
    const bool stream_started = s_stream_started;
    portEXIT_CRITICAL(&s_frame_lock);
    esp_err_t first_error = ESP_OK;

    if (stream_hdl != NULL && stream_started) {
        esp_err_t ret = uvc_host_stream_stop(stream_hdl);
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        } else if (ret == ESP_OK) {
            portENTER_CRITICAL(&s_frame_lock);
            s_stream_started = false;
            portEXIT_CRITICAL(&s_frame_lock);
        }
    }
    esp_err_t ret = uvc_wait_for_app_frames();
    if (ret != ESP_OK && first_error == ESP_OK) {
        first_error = ret;
    }
    if (ret != ESP_OK) {
        return first_error;
    }

    portENTER_CRITICAL(&s_frame_lock);
    const bool still_active = stream_hdl != NULL &&
                              stream_context == s_stream_context &&
                              uvc_active_stream_matches_locked(stream_hdl, stream_generation);
    portEXIT_CRITICAL(&s_frame_lock);
    if (!still_active) {
        return first_error;
    }

    if (stream_hdl != NULL) {
        ret = uvc_host_stream_close(stream_hdl);
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
        if (ret == ESP_OK) {
            uvc_retire_stream_context(stream_context);
            portENTER_CRITICAL(&s_frame_lock);
            s_stream_hdl = NULL;
            s_stream_opened = false;
            s_stream_started = false;
            portEXIT_CRITICAL(&s_frame_lock);
        }
    }
    return first_error;
}

static esp_err_t uvc_recover_locked(test_uvc_recovery_level_t level)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "UVC is not initialized");
    ESP_RETURN_ON_FALSE(level >= TEST_UVC_RECOVERY_STREAM && level <= TEST_UVC_RECOVERY_STACK,
                        ESP_ERR_INVALID_ARG, TAG, "invalid recovery level");

    s_recovery_level = level;
    s_recovery_attempts++;
    s_recovery_required = false;
    uvc_set_state(TEST_UVC_STATE_RECOVERING, ESP_OK);
    ESP_LOGW(TAG, "UVC recovery level=%d attempt=%" PRIu32, (int)level, s_recovery_attempts);

    esp_err_t ret = uvc_close_active_stream();
    if (level == TEST_UVC_RECOVERY_STREAM) {
        if (ret == ESP_OK) {
            ret = test_uvc_run();
        }
    } else if (level == TEST_UVC_RECOVERY_ROOT_PORT) {
        if (s_root_port_powered) {
            esp_err_t power_ret = usb_host_lib_set_root_port_power(false);
            if (power_ret == ESP_OK) {
                s_root_port_powered = false;
            } else if (ret == ESP_OK) {
                ret = power_ret;
            }
        }
        s_device_connected = false;
        s_format_negotiated = false;
        xEventGroupClearBits(s_device_event_group, UVC_DEVICE_CONNECTED_BIT);
        if (ret == ESP_OK) {
            ret = test_uvc_run();
        }
    } else {
        const uint32_t attempts = s_recovery_attempts;
        ret = test_uvc_deinit();
        if (ret == ESP_OK) {
            ret = test_uvc_init();
        }
        s_recovery_level = level;
        s_recovery_attempts = attempts;
        if (ret == ESP_OK) {
            ret = test_uvc_run();
        }
    }

    if (ret != ESP_OK) {
        uvc_set_state(TEST_UVC_STATE_FAILED, ret);
    }
    return ret;
}

esp_err_t test_uvc_recover(test_uvc_recovery_level_t level)
{
    ESP_RETURN_ON_ERROR(uvc_ensure_stream_lock(), TAG, "create UVC stream lock failed");
    ESP_RETURN_ON_FALSE(uvc_stream_lock_take(portMAX_DELAY), ESP_ERR_INVALID_STATE, TAG,
                        "UVC stream lock unavailable");
    esp_err_t ret = uvc_recover_locked(level);
    uvc_stream_lock_give();
    return ret;
}

esp_err_t test_uvc_copy_preview(void *out_rgb565, size_t out_size, test_uvc_preview_info_t *out_info)
{
    const size_t preview_size = TEST_UVC_PREVIEW_H_RES * TEST_UVC_PREVIEW_V_RES * 2U;
    ESP_RETURN_ON_FALSE(out_rgb565 != NULL && out_size >= preview_size, ESP_ERR_INVALID_SIZE,
                        TAG, "preview destination too small");
    if (s_preview_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_preview_lock, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_preview_buf == NULL) {
        xSemaphoreGive(s_preview_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t previous_frame_id = out_info != NULL ? out_info->frame_id : 0;
    if (s_preview_frame_id == 0 || s_preview_frame_id == previous_frame_id) {
        xSemaphoreGive(s_preview_lock);
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(out_rgb565, s_preview_buf, preview_size);
    if (out_info != NULL) {
        *out_info = (test_uvc_preview_info_t) {
            .valid = true,
            .frame_id = s_preview_frame_id,
            .h_res = s_preview_source_w,
            .v_res = s_preview_source_h,
        };
    }
    xSemaphoreGive(s_preview_lock);
    return ESP_OK;
}

static esp_err_t uvc_deinit_locked(void)
{
    if (!s_initialized && !s_usb_host_installed && !s_uvc_driver_installed &&
        s_worker_task_handle == NULL && s_usblib_task_handle == NULL &&
        s_event_queue == NULL && s_device_event_group == NULL) {
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_frame_lock);
    s_stopping = true;
    s_accept_frames = false;
    portEXIT_CRITICAL(&s_frame_lock);
    uvc_set_module_state(TEST_MODULE_STATE_STOPPING, ESP_OK, false);
    esp_err_t first_error = uvc_close_active_stream();
    if (first_error != ESP_OK) {
        ESP_LOGW(TAG, "close UVC stream incomplete: %s", esp_err_to_name(first_error));
    }

    portENTER_CRITICAL(&s_frame_lock);
    uint32_t held_frames = s_frames_held_by_app;
    portEXIT_CRITICAL(&s_frame_lock);
    if (s_stream_hdl != NULL || held_frames != 0) {
        esp_err_t blocker = first_error != ESP_OK ? first_error : ESP_ERR_INVALID_STATE;
        ESP_LOGE(TAG, "UVC stream still owns resources (handle=%p held=%" PRIu32 "); teardown paused",
                 s_stream_hdl, held_frames);
        uvc_set_state(TEST_UVC_STATE_FAILED, blocker);
        return blocker;
    }

    portENTER_CRITICAL(&s_frame_lock);
    s_stream_opened = false;
    s_stream_started = false;
    s_device_connected = false;
    portEXIT_CRITICAL(&s_frame_lock);
    if (s_device_event_group != NULL) {
        xEventGroupClearBits(s_device_event_group, UVC_DEVICE_CONNECTED_BIT);
    }

    if (s_root_port_powered) {
        esp_err_t ret = usb_host_lib_set_root_port_power(false);
        if (ret == ESP_OK) {
            portENTER_CRITICAL(&s_frame_lock);
            s_root_port_powered = false;
            portEXIT_CRITICAL(&s_frame_lock);
        } else {
            if (first_error == ESP_OK) {
                first_error = ret;
            }
            ESP_LOGW(TAG, "power off USB root port failed: %s", esp_err_to_name(ret));
        }
    }

    esp_err_t ret = uvc_stop_worker_task();
    if (ret != ESP_OK) {
        if (first_error == ESP_OK) {
            first_error = ret;
        }
        ESP_LOGE(TAG, "UVC worker did not acknowledge stop; driver resources retained");
        uvc_set_state(TEST_UVC_STATE_FAILED, ret);
        return first_error;
    }

    if (s_uvc_driver_installed) {
        xEventGroupClearBits(s_device_event_group, UVC_USB_ALL_FREE_BIT);
        ret = uvc_host_uninstall();
        if (ret != ESP_OK) {
            if (first_error == ESP_OK) {
                first_error = ret;
            }
            ESP_LOGE(TAG, "uvc_host_uninstall failed; callback dependencies retained: %s",
                     esp_err_to_name(ret));
            uvc_set_state(TEST_UVC_STATE_FAILED, ret);
            return first_error;
        }
        s_uvc_driver_installed = false;
        /* uvc_host_uninstall() joins the driver task, so no callback can still
         * reference a retired static context after this point. */
        uvc_reset_stream_context_pool();
        s_waiting_for_all_free = true;
    }

    if (s_waiting_for_all_free) {
        EventBits_t bits = xEventGroupWaitBits(s_device_event_group, UVC_USB_ALL_FREE_BIT,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
        if ((bits & UVC_USB_ALL_FREE_BIT) == 0) {
            ret = ESP_ERR_TIMEOUT;
            if (first_error == ESP_OK) {
                first_error = ret;
            }
            ESP_LOGE(TAG, "USB Host did not report all devices freed; host task retained");
            uvc_set_state(TEST_UVC_STATE_FAILED, ret);
            return first_error;
        }
        s_waiting_for_all_free = false;
    }

    ret = uvc_stop_usblib_task();
    if (ret != ESP_OK) {
        if (first_error == ESP_OK) {
            first_error = ret;
        }
        ESP_LOGE(TAG, "USB lib task did not acknowledge stop; host resources retained");
        uvc_set_state(TEST_UVC_STATE_FAILED, ret);
        return first_error;
    }

    if (s_usb_host_installed) {
        ret = usb_host_uninstall();
        if (ret != ESP_OK) {
            if (first_error == ESP_OK) {
                first_error = ret;
            }
            portENTER_CRITICAL(&s_frame_lock);
            s_controller_released = false;
            portEXIT_CRITICAL(&s_frame_lock);
            ESP_LOGE(TAG, "usb_host_uninstall failed; controller ownership unknown: %s",
                     esp_err_to_name(ret));
            uvc_set_state(TEST_UVC_STATE_FAILED, ret);
            return first_error;
        }
        s_usb_host_installed = false;
        portENTER_CRITICAL(&s_frame_lock);
        s_controller_released = true;
        portEXIT_CRITICAL(&s_frame_lock);
    }

    if (s_jpeg_decoder != NULL) {
        ret = jpeg_del_decoder_engine(s_jpeg_decoder);
        if (ret != ESP_OK) {
            if (first_error == ESP_OK) {
                first_error = ret;
            }
            ESP_LOGE(TAG, "jpeg_del_decoder_engine failed; application resources retained: %s",
                     esp_err_to_name(ret));
            uvc_set_state(TEST_UVC_STATE_FAILED, ret);
            return first_error;
        }
        s_jpeg_decoder = NULL;
    }

    if (s_ppa_srm != NULL) {
        ret = ppa_unregister_client(s_ppa_srm);
        if (ret != ESP_OK) {
            if (first_error == ESP_OK) {
                first_error = ret;
            }
            ESP_LOGE(TAG, "unregister PPA SRM client failed; buffers retained: %s",
                     esp_err_to_name(ret));
            uvc_set_state(TEST_UVC_STATE_FAILED, ret);
            return first_error;
        }
        s_ppa_srm = NULL;
    }

    uvc_free_app_resources();
    uvc_reset_held_frames();
    if (!s_uvc_driver_installed) {
        uvc_reset_stream_context_pool();
    }

    portENTER_CRITICAL(&s_frame_lock);
    s_initialized = false;
    s_stopping = false;
    s_worker_stop = false;
    s_usblib_stop = false;
    s_accept_frames = false;
    s_stream_opened = false;
    s_stream_started = false;
    s_device_connected = false;
    s_format_negotiated = false;
    s_frame_info_count = 0;
    s_root_port_powered = false;
    s_state = TEST_UVC_STATE_IDLE;
    s_disconnect_pending = false;
    s_disconnect_stream_hdl = NULL;
    s_disconnect_stream_generation = 0;
    s_stream_hdl = NULL;
    portEXIT_CRITICAL(&s_frame_lock);
    uvc_set_module_state(first_error == ESP_OK ? TEST_MODULE_STATE_UNINITIALIZED
                                               : TEST_MODULE_STATE_ERROR,
                         first_error, false);
    ESP_LOGI(TAG, "USB UVC camera test module deinitialized: %s", esp_err_to_name(first_error));
    return first_error;
}

esp_err_t test_uvc_deinit(void)
{
    ESP_RETURN_ON_ERROR(uvc_ensure_stream_lock(), TAG, "create UVC stream lock failed");
    ESP_RETURN_ON_FALSE(uvc_stream_lock_take(portMAX_DELAY), ESP_ERR_INVALID_STATE, TAG,
                        "UVC stream lock unavailable");
    esp_err_t ret = uvc_deinit_locked();
    uvc_stream_lock_give();
    return ret;
}
