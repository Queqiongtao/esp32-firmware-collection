/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "test_uvc.h"

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
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "test_config.h"
#include "driver/jpeg_decode.h"
#include "driver/jpeg_types.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

static const char *TAG = "test_uvc";

#define UVC_DEVICE_CONNECTED_BIT BIT0
#define UVC_USB_ALL_FREE_BIT     BIT1

typedef struct {
    uvc_host_frame_t *frame;
} uvc_app_queue_msg_t;

static QueueHandle_t s_event_queue;
static EventGroupHandle_t s_device_event_group;
static TaskHandle_t s_usblib_task_handle;
static TaskHandle_t s_worker_task_handle;
static jpeg_decoder_handle_t s_jpeg_decoder;
static uvc_host_stream_hdl_t s_stream_hdl;
static uint8_t *s_rgb565_buf;
static size_t s_rgb565_buf_size;
static uint8_t *s_preview_buf;
static SemaphoreHandle_t s_preview_lock;
static uint32_t s_preview_frame_id;
static uint32_t s_preview_source_w;
static uint32_t s_preview_source_h;
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
static bool s_stream_opened;
static bool s_stream_started;
static bool s_device_connected;
static bool s_accept_frames;
static portMUX_TYPE s_frame_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_frames_held_by_app;

static uint32_t s_frames_received;
static uint32_t s_frames_decoded;
static uint32_t s_decode_errors;
static uint32_t s_last_frame_bytes;
static uint32_t s_last_decode_us;
static uint32_t s_last_frame_w;
static uint32_t s_last_frame_h;
static esp_err_t s_last_error = ESP_OK;
static int64_t s_stats_window_start_us;
static uint32_t s_stats_window_received;
static uint32_t s_stats_window_decoded;
static uint32_t s_stats_window_errors;

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

    if (now_us - s_stats_window_start_us < 1000000) {
        return;
    }

    ESP_LOGI(TAG, "UVC stats: received=%" PRIu32 " decoded=%" PRIu32 " errors=%" PRIu32
             " last=%" PRIu32 "x%" PRIu32 " len=%" PRIu32 " decode=%" PRIu32 "us",
             s_frames_received - s_stats_window_received,
             s_frames_decoded - s_stats_window_decoded,
             s_decode_errors - s_stats_window_errors,
             s_last_frame_w, s_last_frame_h, s_last_frame_bytes, s_last_decode_us);

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

/* Enumerate the device's frame descriptors and pick the best MJPEG format.
 * Preference: MJPEG format -> preferred resolution -> highest fps.
 * Fallback: if preferred resolution is unavailable, the largest MJPEG
 * resolution supported by the device; if no MJPEG at all, the first entry. */
static void uvc_negotiate_format(uint8_t dev_addr, uint8_t uvc_stream_index)
{
    s_format_negotiated = false;
    s_frame_info_count = 0;

    size_t list_size = TEST_UVC_FRAME_INFO_MAX;
    esp_err_t ret = uvc_host_get_frame_list(dev_addr, uvc_stream_index,
                                            &s_frame_info_list, &list_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "uvc_host_get_frame_list failed: %s (dev_addr=%u stream=%u)",
                 esp_err_to_name(ret), (unsigned)dev_addr, (unsigned)uvc_stream_index);
        return;
    }

    if (list_size == 0) {
        ESP_LOGW(TAG, "device reported 0 frame descriptors");
        return;
    }

    if (list_size > TEST_UVC_FRAME_INFO_MAX) {
        ESP_LOGW(TAG, "device reported %zu frame descriptors, only %d enumerated",
                 list_size, TEST_UVC_FRAME_INFO_MAX);
        list_size = TEST_UVC_FRAME_INFO_MAX;
    }
    s_frame_info_count = list_size;

    ESP_LOGI(TAG, "enumerated %zu frame descriptors:", list_size);
    for (size_t i = 0; i < list_size; ++i) {
        const uvc_host_frame_info_t *f = &s_frame_info_list[i];
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
        const uvc_host_frame_info_t *f = &s_frame_info_list[i];
        if (f->format != TEST_UVC_FORMAT) {
            continue;
        }

        uint32_t interval = uvc_pick_best_interval(f);
        uint32_t pixels = f->h_res * f->v_res;

        uint32_t preferred_h_res = s_requested_resolution_valid ? s_requested_h_res : TEST_UVC_PREFER_H_RES;
        uint32_t preferred_v_res = s_requested_resolution_valid ? s_requested_v_res : TEST_UVC_PREFER_V_RES;
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
    if (chosen == NULL && list_size > 0) {
        /* No MJPEG at all: use the first entry as last-resort fallback. */
        chosen = &s_frame_info_list[0];
        chosen_interval = uvc_pick_best_interval(chosen);
    }

    if (chosen == NULL) {
        ESP_LOGE(TAG, "no usable frame descriptor after negotiation");
        return;
    }

    s_negotiated_format.h_res = chosen->h_res;
    s_negotiated_format.v_res = chosen->v_res;
    s_negotiated_format.fps = uvc_interval_to_fps(chosen_interval);
    s_negotiated_format.format = chosen->format;
    s_format_negotiated = true;

    ESP_LOGI(TAG, "negotiated format: %ux%u@%.1ffps format=%d%s",
             (unsigned)s_negotiated_format.h_res, (unsigned)s_negotiated_format.v_res,
             s_negotiated_format.fps, (int)s_negotiated_format.format,
             (chosen == best_preferred) ? " (preferred)" : " (fallback)");
}

static void uvc_update_preview(uint32_t src_w, uint32_t src_h, uint32_t out_size)
{
    if (s_preview_buf == NULL || s_preview_lock == NULL || src_w == 0 || src_h == 0) {
        return;
    }

    const size_t source_stride = out_size / (src_h * 2U);
    if (source_stride < src_w) {
        ESP_LOGW(TAG, "decoded RGB565 stride invalid: width=%u stride=%zu out=%u",
                 (unsigned)src_w, source_stride, (unsigned)out_size);
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

    memset(s_preview_buf, 0, TEST_UVC_PREVIEW_H_RES * TEST_UVC_PREVIEW_V_RES * 2U);
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
    esp_cache_msync(s_preview_buf, TEST_UVC_PREVIEW_H_RES * TEST_UVC_PREVIEW_V_RES * 2U,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    s_preview_source_w = src_w;
    s_preview_source_h = src_h;
    s_preview_frame_id++;
    xSemaphoreGive(s_preview_lock);
}

static void uvc_driver_event_callback(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;

    switch (event->type) {
    case UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED:
        s_device_connected = true;
        ESP_LOGI(TAG, "UVC driver: compatible device connected, dev_addr=%u stream_index=%u frame_info_num=%u",
                 event->device_connected.dev_addr,
                 event->device_connected.uvc_stream_index,
                 (unsigned)event->device_connected.frame_info_num);
        uvc_negotiate_format(event->device_connected.dev_addr,
                             event->device_connected.uvc_stream_index);
        xEventGroupSetBits(s_device_event_group, UVC_DEVICE_CONNECTED_BIT);
        break;
    default:
        ESP_LOGW(TAG, "UVC driver: unknown event %d", event->type);
        break;
    }
}

static void uvc_stream_event_callback(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;

    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        s_last_error = event->transfer_error.error;
        ESP_LOGE(TAG, "UVC stream transfer error: %s", esp_err_to_name(event->transfer_error.error));
        break;
    case UVC_HOST_DEVICE_DISCONNECTED: {
        uvc_host_stream_hdl_t hdl = event->device_disconnected.stream_hdl;
        ESP_LOGI(TAG, "UVC device disconnected, closing stream");
        s_device_connected = false;
        s_format_negotiated = false;
        s_frame_info_count = 0;
        s_stream_started = false;
        xEventGroupClearBits(s_device_event_group, UVC_DEVICE_CONNECTED_BIT);
        if (hdl != NULL) {
            esp_err_t ret = uvc_host_stream_close(hdl);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "uvc_host_stream_close on disconnect failed: %s", esp_err_to_name(ret));
            }
        }
        if (hdl == s_stream_hdl) {
            s_stream_hdl = NULL;
        }
        s_stream_opened = false;
        break;
    }
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        ESP_LOGW(TAG, "UVC frame buffer overflow (frame exceeded allocated size)");
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        ESP_LOGW(TAG, "UVC frame buffer underflow (no free buffer available)");
        break;
    default:
        ESP_LOGW(TAG, "UVC stream: unsupported event %d (possibly suspend/resume)", event->type);
        break;
    }
}

static bool uvc_frame_callback(const uvc_host_frame_t *frame, void *user_ctx)
{
    (void)user_ctx;

    if (s_event_queue == NULL || s_stopping || !s_accept_frames) {
        return true;
    }

    uvc_host_frame_t *frame_ptr = (uvc_host_frame_t *)frame;
    portENTER_CRITICAL(&s_frame_lock);
    s_frames_held_by_app++;
    portEXIT_CRITICAL(&s_frame_lock);
    if (xQueueSend(s_event_queue, &frame_ptr, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_frame_lock);
        s_frames_held_by_app--;
        portEXIT_CRITICAL(&s_frame_lock);
        ESP_LOGW(TAG, "frame queue full, dropping frame");
        return true;
    }
    return false;
}

static void uvc_return_frame(uvc_host_frame_t *frame)
{
    if (frame != NULL && s_stream_hdl != NULL) {
        esp_err_t ret = uvc_host_frame_return(s_stream_hdl, frame);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "uvc_host_frame_return failed: %s", esp_err_to_name(ret));
        }
    }
    portENTER_CRITICAL(&s_frame_lock);
    if (s_frames_held_by_app > 0) {
        s_frames_held_by_app--;
    }
    portEXIT_CRITICAL(&s_frame_lock);
}

static void uvc_ensure_rgb565_buf(uint32_t width, uint32_t height)
{
    size_t need = ((width + 15U) / 16U) * 16U * ((height + 15U) / 16U) * 16U * 2U;
    if (s_rgb565_buf != NULL && s_rgb565_buf_size >= need) {
        return;
    }

    if (s_rgb565_buf != NULL) {
        free(s_rgb565_buf);
        s_rgb565_buf = NULL;
        s_rgb565_buf_size = 0;
    }

    const jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    s_rgb565_buf = jpeg_alloc_decoder_mem(need, &mem_cfg, &s_rgb565_buf_size);
    if (s_rgb565_buf != NULL) {
        ESP_LOGI(TAG, "allocated RGB565 decode buffer: requested=%zu allocated=%zu bytes for %ux%u",
                 need, s_rgb565_buf_size, (unsigned)width, (unsigned)height);
    } else {
        ESP_LOGE(TAG, "failed to allocate RGB565 decode buffer: %zu bytes", need);
    }
}

static void uvc_worker_task(void *arg)
{
    (void)arg;

    uvc_app_queue_msg_t msg;
    while (true) {
        if (xQueueReceive(s_event_queue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
            if (s_stopping) {
                break;
            }
            continue;
        }

        uvc_host_frame_t *frame = msg.frame;
        if (frame == NULL) {
            continue;
        }

        s_frames_received++;
        s_last_frame_bytes = (uint32_t)frame->data_len;

        if (frame->data == NULL || frame->data_len == 0) {
            ESP_LOGW(TAG, "frame %u: empty payload", (unsigned)s_frames_received);
            uvc_return_frame(frame);
            uvc_log_stats_if_due();
            continue;
        }

        if (s_jpeg_decoder == NULL) {
            ESP_LOGE(TAG, "frame %u: jpeg decoder not available", (unsigned)s_frames_received);
            uvc_return_frame(frame);
            uvc_log_stats_if_due();
            continue;
        }

        jpeg_decode_picture_info_t info = {0};
        esp_err_t ret = jpeg_decoder_get_info(frame->data, (uint32_t)frame->data_len, &info);
        if (ret != ESP_OK) {
            s_decode_errors++;
            s_last_error = ret;
            ESP_LOGW(TAG, "frame %u: jpeg_decoder_get_info failed: %s (len=%u)",
                     (unsigned)s_frames_received, esp_err_to_name(ret), (unsigned)frame->data_len);
            uvc_return_frame(frame);
            uvc_log_stats_if_due();
            continue;
        }

        s_last_frame_w = info.width;
        s_last_frame_h = info.height;
        uvc_ensure_rgb565_buf(info.width, info.height);
        if (s_rgb565_buf == NULL) {
            s_decode_errors++;
            uvc_return_frame(frame);
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
        s_last_decode_us = (uint32_t)(esp_timer_get_time() - start_us);

        if (ret != ESP_OK) {
            s_decode_errors++;
            s_last_error = ret;
            ESP_LOGW(TAG, "frame %u: jpeg decode failed: %s (len=%u %ux%u)",
                     (unsigned)s_frames_received, esp_err_to_name(ret),
                     (unsigned)frame->data_len, (unsigned)info.width, (unsigned)info.height);
        } else {
            s_frames_decoded++;
        }

        uvc_return_frame(frame);
        if (ret == ESP_OK) {
            int64_t now_us = esp_timer_get_time();
            if (now_us - s_preview_last_update_us >= TEST_UVC_PREVIEW_UPDATE_INTERVAL_MS * 1000LL) {
                s_preview_last_update_us = now_us;
                uvc_update_preview(info.width, info.height, out_size);
            }
        }
        uvc_log_stats_if_due();
    }

    s_worker_task_handle = NULL;
    vTaskDelete(NULL);
}

static void uvc_drain_queued_frames(void)
{
    uvc_app_queue_msg_t msg;
    while (s_event_queue != NULL && xQueueReceive(s_event_queue, &msg, 0) == pdTRUE) {
        uvc_return_frame(msg.frame);
    }
}

static esp_err_t uvc_wait_for_app_frames(void)
{
    for (uint32_t i = 0; i < 50; ++i) {
        uvc_drain_queued_frames();
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

    while (!s_stopping) {
        uint32_t event_flags = 0;
        esp_err_t ret = usb_host_lib_handle_events(pdMS_TO_TICKS(100), &event_flags);
        if (ret == ESP_ERR_NOT_FINISHED) {
            break;
        }
        /* ESP_ERR_TIMEOUT 表示无事件（空闲），不是错误；过滤掉避免刷屏。
         * 只对其它非 OK 返回告警。 */
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "usb_host_lib_handle_events: %s", esp_err_to_name(ret));
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB Host: all devices freed");
            xEventGroupSetBits(s_device_event_group, UVC_USB_ALL_FREE_BIT);
        }
    }

    s_usblib_task_handle = NULL;
    vTaskDelete(NULL);
}

static void uvc_cleanup_app_resources(void)
{
    if (s_rgb565_buf != NULL) {
        free(s_rgb565_buf);
        s_rgb565_buf = NULL;
        s_rgb565_buf_size = 0;
    }

    if (s_preview_buf != NULL) {
        heap_caps_free(s_preview_buf);
        s_preview_buf = NULL;
    }
    if (s_preview_lock != NULL) {
        vSemaphoreDelete(s_preview_lock);
        s_preview_lock = NULL;
    }

    if (s_worker_task_handle != NULL) {
        vTaskDelete(s_worker_task_handle);
        s_worker_task_handle = NULL;
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

esp_err_t test_uvc_init(void)
{
    esp_err_t ret = ESP_OK;

    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing USB UVC camera test module on High-Speed USB OTG / UTMI PHY (peripheral_map=0x%x)",
             (unsigned)TEST_UVC_USB_HOST_PERIPHERAL_MAP);
    s_stopping = false;

    s_event_queue = xQueueCreate(TEST_UVC_FRAME_QUEUE_LEN, sizeof(uvc_app_queue_msg_t));
    ESP_RETURN_ON_FALSE(s_event_queue != NULL, ESP_ERR_NO_MEM, TAG, "create UVC frame queue failed");

    s_device_event_group = xEventGroupCreate();
    ESP_GOTO_ON_FALSE(s_device_event_group != NULL, ESP_ERR_NO_MEM, err_queue,
                      TAG, "create UVC device event group failed");

    s_preview_lock = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(s_preview_lock != NULL, ESP_ERR_NO_MEM, err_queue,
                      TAG, "create UVC preview lock failed");
    s_preview_buf = heap_caps_calloc(TEST_UVC_PREVIEW_H_RES * TEST_UVC_PREVIEW_V_RES,
                                     sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_GOTO_ON_FALSE(s_preview_buf != NULL, ESP_ERR_NO_MEM, err_queue,
                      TAG, "allocate UVC preview buffer failed");
    s_preview_frame_id = 0;
    s_preview_source_w = 0;
    s_preview_source_h = 0;
    s_preview_last_update_us = 0;
    s_frame_info_count = 0;
    s_accept_frames = false;
    portENTER_CRITICAL(&s_frame_lock);
    s_frames_held_by_app = 0;
    portEXIT_CRITICAL(&s_frame_lock);

    s_stream_opened = false;
    s_stream_started = false;
    s_device_connected = false;
    s_format_negotiated = false;
    s_frames_received = 0;
    s_frames_decoded = 0;
    s_decode_errors = 0;
    s_last_frame_bytes = 0;
    s_last_decode_us = 0;
    s_last_frame_w = 0;
    s_last_frame_h = 0;
    s_last_error = ESP_OK;
    s_stats_window_start_us = 0;
    s_stats_window_received = 0;
    s_stats_window_decoded = 0;
    s_stats_window_errors = 0;

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        .peripheral_map = TEST_UVC_USB_HOST_PERIPHERAL_MAP,
    };
    ESP_GOTO_ON_ERROR(usb_host_install(&host_config), err_queue, TAG, "usb_host_install failed");

    BaseType_t task_created = xTaskCreatePinnedToCore(uvc_usblib_task,
                                                      "uvc_usblib",
                                                      TEST_UVC_USB_LIB_TASK_STACK,
                                                      NULL,
                                                      TEST_UVC_USB_LIB_TASK_PRIORITY,
                                                      &s_usblib_task_handle,
                                                      TEST_UVC_USB_LIB_TASK_CORE);
    ESP_GOTO_ON_FALSE(task_created == pdTRUE, ESP_ERR_NO_MEM, err_uninstall_host, TAG, "create USB lib task failed");

    const uvc_host_driver_config_t uvc_driver_config = {
        .driver_task_stack_size = TEST_UVC_DRIVER_TASK_STACK,
        .driver_task_priority = TEST_UVC_DRIVER_TASK_PRIORITY,
        .xCoreID = TEST_UVC_DRIVER_TASK_CORE,
        .create_background_task = true,
        .event_cb = uvc_driver_event_callback,
        .user_ctx = NULL,
    };
    ESP_GOTO_ON_ERROR(uvc_host_install(&uvc_driver_config), err_del_usblib_task, TAG, "uvc_host_install failed");

    const jpeg_decode_engine_cfg_t jpeg_dec_cfg = {
        .intr_priority = 0,
        .timeout_ms = TEST_UVC_JPEG_DECODE_TIMEOUT_MS,
        .flags = { .allow_pd = 0 },
    };
    uvc_log_memory("before JPEG decoder");
    ESP_GOTO_ON_ERROR(jpeg_new_decoder_engine(&jpeg_dec_cfg, &s_jpeg_decoder),
                      err_uninstall_uvc, TAG, "jpeg_new_decoder_engine failed");
    uvc_log_memory("after JPEG decoder");

    task_created = xTaskCreatePinnedToCore(uvc_worker_task,
                                          "uvc_worker",
                                          TEST_UVC_WORKER_TASK_STACK,
                                          NULL,
                                          TEST_UVC_WORKER_TASK_PRIORITY,
                                          &s_worker_task_handle,
                                          TEST_UVC_WORKER_TASK_CORE);
    ESP_GOTO_ON_FALSE(task_created == pdTRUE, ESP_ERR_NO_MEM, err_del_jpeg, TAG, "create UVC worker task failed");

    s_initialized = true;
    ESP_LOGI(TAG, "UVC camera test module initialized, waiting for UVC device (vid=0x%x pid=0x%x prefer %ux%u MJPEG)",
             (unsigned)TEST_UVC_VID, (unsigned)TEST_UVC_PID,
             (unsigned)TEST_UVC_PREFER_H_RES, (unsigned)TEST_UVC_PREFER_V_RES);
    return ESP_OK;

err_del_jpeg:
    jpeg_del_decoder_engine(s_jpeg_decoder);
    s_jpeg_decoder = NULL;
err_uninstall_uvc:
    uvc_host_uninstall();
err_del_usblib_task:
    s_stopping = true;
    usb_host_lib_unblock();
    for (int i = 0; i < 10 && s_usblib_task_handle != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_usblib_task_handle != NULL) {
        vTaskDelete(s_usblib_task_handle);
        s_usblib_task_handle = NULL;
    }
err_uninstall_host:
    usb_host_uninstall();
err_queue:
    uvc_cleanup_app_resources();
    return ret;
}

esp_err_t test_uvc_run(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "UVC not initialized, call test_uvc_init first");

    if (s_stream_opened) {
        if (s_stream_started) {
            return ESP_OK;
        }
        ESP_RETURN_ON_ERROR(uvc_host_stream_start(s_stream_hdl), TAG, "uvc_host_stream_start failed");
        s_stream_started = true;
        s_accept_frames = true;
        return ESP_OK;
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
        vTaskDelay(pdMS_TO_TICKS(TEST_UVC_ROOT_PORT_CYCLE_DELAY_MS));
        s_device_connected = false;
        s_format_negotiated = false;
        xEventGroupClearBits(s_device_event_group, UVC_DEVICE_CONNECTED_BIT);
        ESP_RETURN_ON_ERROR(usb_host_lib_set_root_port_power(true), TAG,
                            "restart UVC root port failed");
    }

    ESP_RETURN_ON_FALSE(connected, ESP_ERR_NOT_FOUND, TAG,
                        "UVC device did not enumerate after %u attempts",
                        (unsigned)TEST_UVC_ENUM_MAX_ATTEMPTS);

    ESP_LOGI(TAG, "waiting %u ms for UVC device control endpoint to stabilize",
             (unsigned)TEST_UVC_DEVICE_READY_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(TEST_UVC_DEVICE_READY_DELAY_MS));

    const uvc_host_stream_config_t stream_config = {
        .event_cb = uvc_stream_event_callback,
        .frame_cb = uvc_frame_callback,
        .user_ctx = NULL,
        .usb = {
            .dev_addr = UVC_HOST_ANY_DEV_ADDR,
            .vid = TEST_UVC_VID,
            .pid = TEST_UVC_PID,
            .uvc_stream_index = TEST_UVC_UVC_STREAM_INDEX,
        },
        .vs_format = s_format_negotiated ? s_negotiated_format
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

    esp_err_t ret = ESP_FAIL;
    for (uint32_t attempt = 1; attempt <= TEST_UVC_STREAM_OPEN_MAX_ATTEMPTS; ++attempt) {
        ret = uvc_host_stream_open(&stream_config, 0, &s_stream_hdl);
        if (ret == ESP_OK) {
            break;
        }

        s_stream_hdl = NULL;
        if (ret != ESP_ERR_INVALID_RESPONSE || attempt == TEST_UVC_STREAM_OPEN_MAX_ATTEMPTS) {
            s_last_error = ret;
            ESP_LOGE(TAG, "uvc_host_stream_open failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGW(TAG, "UVC stream negotiation failed (attempt %u/%u), retrying",
                 (unsigned)attempt, (unsigned)TEST_UVC_STREAM_OPEN_MAX_ATTEMPTS);
        vTaskDelay(pdMS_TO_TICKS(TEST_UVC_STREAM_OPEN_RETRY_DELAY_MS));
    }

    s_stream_opened = true;
    ESP_RETURN_ON_ERROR(uvc_host_stream_start(s_stream_hdl), TAG, "uvc_host_stream_start failed");
    s_stream_started = true;
    s_accept_frames = true;
    ESP_LOGI(TAG, "UVC stream started");
    return ESP_OK;
}

esp_err_t test_uvc_get_status(test_uvc_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != NULL, ESP_ERR_INVALID_ARG, TAG, "out_status is NULL");

    *out_status = (test_uvc_status_t) {
        .initialized = s_initialized,
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
        .last_frame_bytes = s_last_frame_bytes,
        .last_decode_us = s_last_decode_us,
        .last_frame_w = s_last_frame_w,
        .last_frame_h = s_last_frame_h,
        .last_error = s_last_error,
    };
    return ESP_OK;
}

esp_err_t test_uvc_get_resolutions(test_uvc_resolution_t *out_list, size_t *inout_count)
{
    ESP_RETURN_ON_FALSE(inout_count != NULL, ESP_ERR_INVALID_ARG, TAG, "inout_count is NULL");

    size_t total = 0;
    for (size_t i = 0; i < s_frame_info_count; ++i) {
        const uvc_host_frame_info_t *info = &s_frame_info_list[i];
        if (info->format != TEST_UVC_FORMAT) {
            continue;
        }
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j) {
            if (s_frame_info_list[j].format == TEST_UVC_FORMAT &&
                s_frame_info_list[j].h_res == info->h_res &&
                s_frame_info_list[j].v_res == info->v_res) {
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
    s_requested_h_res = h_res;
    s_requested_v_res = v_res;
    s_requested_resolution_valid = true;
    return ESP_OK;
}

esp_err_t test_uvc_change_resolution(uint32_t h_res, uint32_t v_res)
{
    ESP_RETURN_ON_ERROR(test_uvc_set_preferred_resolution(h_res, v_res), TAG,
                        "set UVC preferred resolution failed");
    ESP_RETURN_ON_FALSE(s_initialized && s_stream_hdl != NULL && s_stream_opened,
                        ESP_ERR_INVALID_STATE, TAG, "UVC stream is not open");

    const uvc_host_frame_info_t *selected = NULL;
    uint32_t selected_interval = 0;
    for (size_t i = 0; i < s_frame_info_count; ++i) {
        const uvc_host_frame_info_t *info = &s_frame_info_list[i];
        if (info->format != TEST_UVC_FORMAT || info->h_res != h_res || info->v_res != v_res) {
            continue;
        }
        uint32_t interval = uvc_pick_best_interval(info);
        if (selected == NULL || interval < selected_interval) {
            selected = info;
            selected_interval = interval;
        }
    }
    ESP_RETURN_ON_FALSE(selected != NULL, ESP_ERR_NOT_FOUND, TAG,
                        "requested UVC resolution %ux%u is unavailable",
                        (unsigned)h_res, (unsigned)v_res);

    s_accept_frames = false;
    if (s_stream_started) {
        esp_err_t ret = uvc_host_stream_stop(s_stream_hdl);
        if (ret != ESP_OK) {
            s_last_error = ret;
            return ret;
        }
        s_stream_started = false;
    }
    ESP_RETURN_ON_ERROR(uvc_wait_for_app_frames(), TAG, "UVC frames did not return before format change");

    uvc_host_stream_format_t format = {
        .h_res = selected->h_res,
        .v_res = selected->v_res,
        .fps = uvc_interval_to_fps(selected_interval),
        .format = selected->format,
    };
    ESP_RETURN_ON_ERROR(uvc_host_stream_format_select(s_stream_hdl, &format), TAG,
                        "select UVC stream format failed");
    ESP_RETURN_ON_ERROR(uvc_host_stream_start(s_stream_hdl), TAG, "restart UVC stream failed");

    s_negotiated_format = format;
    s_format_negotiated = true;
    s_stream_started = true;
    s_accept_frames = true;
    s_last_error = ESP_OK;
    ESP_LOGI(TAG, "UVC stream changed to %ux%u@%.1fFPS", (unsigned)format.h_res,
             (unsigned)format.v_res, format.fps);
    return ESP_OK;
}

esp_err_t test_uvc_copy_preview(void *out_rgb565, size_t out_size, test_uvc_preview_info_t *out_info)
{
    const size_t preview_size = TEST_UVC_PREVIEW_H_RES * TEST_UVC_PREVIEW_V_RES * 2U;
    ESP_RETURN_ON_FALSE(out_rgb565 != NULL && out_size >= preview_size, ESP_ERR_INVALID_SIZE,
                        TAG, "preview destination too small");
    if (s_preview_lock == NULL || s_preview_buf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_preview_lock, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_preview_frame_id == 0) {
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

esp_err_t test_uvc_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    s_stopping = true;
    s_accept_frames = false;

    if (s_stream_hdl != NULL) {
        if (s_stream_started) {
            esp_err_t ret = uvc_host_stream_stop(s_stream_hdl);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "uvc_host_stream_stop failed: %s", esp_err_to_name(ret));
                s_last_error = ret;
            }
            s_stream_started = false;
        }
        esp_err_t ret = uvc_wait_for_app_frames();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "UVC frames did not return before close: %s", esp_err_to_name(ret));
            s_last_error = ret;
            return ret;
        }
        ret = uvc_host_stream_close(s_stream_hdl);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "uvc_host_stream_close failed: %s", esp_err_to_name(ret));
            s_last_error = ret;
            return ret;
        }
        s_stream_hdl = NULL;
    }
    s_stream_opened = false;
    s_device_connected = false;
    xEventGroupClearBits(s_device_event_group, UVC_DEVICE_CONNECTED_BIT);

    xEventGroupClearBits(s_device_event_group, UVC_USB_ALL_FREE_BIT);
    esp_err_t ret = uvc_host_uninstall();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "uvc_host_uninstall failed: %s", esp_err_to_name(ret));
        s_last_error = ret;
        return ret;
    }

    EventBits_t bits = xEventGroupWaitBits(s_device_event_group, UVC_USB_ALL_FREE_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
    if ((bits & UVC_USB_ALL_FREE_BIT) == 0) {
        ESP_LOGW(TAG, "USB Host did not report all devices freed");
        s_last_error = ESP_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;
    }

    usb_host_lib_unblock();
    for (int i = 0; i < 10 && s_usblib_task_handle != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_usblib_task_handle != NULL) {
        ESP_LOGW(TAG, "USB lib task did not exit in time");
        s_last_error = ESP_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;
    }

    for (int i = 0; i < 10 && s_worker_task_handle != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_worker_task_handle != NULL) {
        ESP_LOGW(TAG, "UVC worker task did not exit in time");
        s_last_error = ESP_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;
    }

    ret = usb_host_uninstall();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "usb_host_uninstall failed: %s", esp_err_to_name(ret));
        s_last_error = ret;
        return ret;
    }

    if (s_jpeg_decoder != NULL) {
        ret = jpeg_del_decoder_engine(s_jpeg_decoder);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "jpeg_del_decoder_engine failed: %s", esp_err_to_name(ret));
        }
        s_jpeg_decoder = NULL;
    }

    uvc_cleanup_app_resources();

    s_initialized = false;
    s_stopping = false;
    s_accept_frames = false;
    s_stream_opened = false;
    s_stream_started = false;
    s_device_connected = false;
    s_format_negotiated = false;
    s_frame_info_count = 0;
    s_frames_received = 0;
    s_frames_decoded = 0;
    s_decode_errors = 0;
    s_last_frame_bytes = 0;
    s_last_decode_us = 0;
    s_last_frame_w = 0;
    s_last_frame_h = 0;
    s_last_error = ESP_OK;
    ESP_LOGI(TAG, "USB UVC camera test module deinitialized");
    return ESP_OK;
}
