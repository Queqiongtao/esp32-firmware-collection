/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "display_lvgl.h"

#include <inttypes.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>
#include "sdkconfig.h"
#include "display_board.h"
#include "display_config.h"
#include "display_touch.h"
#include "esp_check.h"
#include "esp_efuse.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display_lvgl";
static _lock_t s_lvgl_api_lock;

extern void example_lvgl_demo_ui(lv_display_t *disp);

#if CONFIG_EXAMPLE_LVGL_PERF_LOG
typedef struct {
    uint32_t report_start_ms;
    uint32_t refr_start_ms;
    uint32_t render_start_ms;
    uint32_t flush_start_ms;
    uint32_t refr_elapsed_ms;
    uint32_t render_elapsed_ms;
    uint32_t flush_elapsed_ms;
    uint32_t refr_count;
    uint32_t render_count;
    uint32_t flush_count;
    bool flush_active;
} display_lvgl_perf_t;

static display_lvgl_perf_t s_lvgl_perf;

static uint32_t display_lvgl_avg_time(uint32_t elapsed_ms, uint32_t count)
{
    return count > 0 ? elapsed_ms / count : 0;
}

static void display_lvgl_perf_report_if_ready(void)
{
    const uint32_t now_ms = lv_tick_get();

    if (s_lvgl_perf.report_start_ms == 0) {
        s_lvgl_perf.report_start_ms = now_ms;
        return;
    }

    const uint32_t report_elapsed_ms = lv_tick_elaps(s_lvgl_perf.report_start_ms);
    if (report_elapsed_ms < 1000) {
        return;
    }

    const uint32_t fps = (s_lvgl_perf.refr_count * 1000) / report_elapsed_ms;
    const uint32_t pure_render_elapsed_ms = s_lvgl_perf.render_elapsed_ms > s_lvgl_perf.flush_elapsed_ms ?
                                            s_lvgl_perf.render_elapsed_ms - s_lvgl_perf.flush_elapsed_ms : 0;
    const uint32_t refr_avg_ms = display_lvgl_avg_time(s_lvgl_perf.refr_elapsed_ms, s_lvgl_perf.refr_count);
    const uint32_t render_avg_ms = display_lvgl_avg_time(pure_render_elapsed_ms, s_lvgl_perf.render_count);
    const uint32_t flush_avg_ms = display_lvgl_avg_time(s_lvgl_perf.flush_elapsed_ms, s_lvgl_perf.flush_count);

    ESP_LOGI(TAG, "LVGL perf: fps=%" PRIu32 " refr=%" PRIu32 "ms render=%" PRIu32 "ms flush=%" PRIu32
             "ms refr_cnt=%" PRIu32 " render_cnt=%" PRIu32 " flush_cnt=%" PRIu32,
             fps, refr_avg_ms, render_avg_ms, flush_avg_ms,
             s_lvgl_perf.refr_count, s_lvgl_perf.render_count, s_lvgl_perf.flush_count);

    s_lvgl_perf.refr_elapsed_ms = 0;
    s_lvgl_perf.render_elapsed_ms = 0;
    s_lvgl_perf.flush_elapsed_ms = 0;
    s_lvgl_perf.refr_count = 0;
    s_lvgl_perf.render_count = 0;
    s_lvgl_perf.flush_count = 0;
    s_lvgl_perf.report_start_ms = now_ms;
}

static void display_lvgl_perf_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);

    switch (code) {
    case LV_EVENT_REFR_START:
        s_lvgl_perf.refr_start_ms = lv_tick_get();
        break;
    case LV_EVENT_REFR_READY:
        s_lvgl_perf.refr_elapsed_ms += lv_tick_elaps(s_lvgl_perf.refr_start_ms);
        s_lvgl_perf.refr_count++;
        display_lvgl_perf_report_if_ready();
        break;
    case LV_EVENT_RENDER_START:
        s_lvgl_perf.render_start_ms = lv_tick_get();
        break;
    case LV_EVENT_RENDER_READY:
        s_lvgl_perf.render_elapsed_ms += lv_tick_elaps(s_lvgl_perf.render_start_ms);
        s_lvgl_perf.render_count++;
        break;
    case LV_EVENT_FLUSH_START:
    case LV_EVENT_FLUSH_WAIT_START:
        s_lvgl_perf.flush_start_ms = lv_tick_get();
        s_lvgl_perf.flush_active = true;
        break;
    case LV_EVENT_FLUSH_FINISH:
    case LV_EVENT_FLUSH_WAIT_FINISH:
        if (s_lvgl_perf.flush_active) {
            s_lvgl_perf.flush_elapsed_ms += lv_tick_elaps(s_lvgl_perf.flush_start_ms);
            s_lvgl_perf.flush_count++;
            s_lvgl_perf.flush_active = false;
        }
        break;
    default:
        break;
    }
}
#endif

#if CONFIG_EXAMPLE_USE_DMA2D_COPY_FRAME
static void display_lvgl_rounder_flush_area_cb(lv_event_t *event)
{
    lv_area_t *area = lv_event_get_invalidated_area(event);
    area->x1 = DISPLAY_ALIGN_DOWN(area->x1, 16);
    area->x2 = DISPLAY_ALIGN_UP(area->x2, 16) - 1;
}
#endif

static void display_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

static void display_lvgl_increase_tick(void *arg)
{
    (void)arg;
    lv_tick_inc(DISPLAY_LVGL_TICK_PERIOD_MS);
}

static void display_lvgl_port_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Starting LVGL task");

    while (1) {
        uint32_t time_till_next_ms;

        _lock_acquire(&s_lvgl_api_lock);
        time_till_next_ms = lv_timer_handler();
        _lock_release(&s_lvgl_api_lock);

        time_till_next_ms = MAX(time_till_next_ms, DISPLAY_LVGL_TASK_MIN_DELAY_MS);
        time_till_next_ms = MIN(time_till_next_ms, DISPLAY_LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);
    }
}

static bool display_lvgl_notify_flush_ready(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;

    (void)panel;
    (void)edata;
    lv_display_flush_ready(disp);
    return false;
}

static size_t display_lvgl_get_buffer_alignment(void)
{
    size_t alignment = 1;

#if CONFIG_EXAMPLE_USE_DMA2D_COPY_FRAME
    if (esp_efuse_is_flash_encryption_enabled()) {
        alignment = SOC_GDMA_EXT_MEM_ENC_ALIGNMENT;
        if ((DISPLAY_PANEL_H_RES % alignment) != 0) {
            ESP_LOGW(TAG, "display width is not aligned to %u bytes, MSPI error may occur", (unsigned int)alignment);
        }
    }
#endif

    return alignment;
}

static esp_err_t display_lvgl_allocate_buffers(lv_display_t *display)
{
    const size_t alignment = display_lvgl_get_buffer_alignment();
    const size_t draw_buffer_sz = DISPLAY_PANEL_H_RES * DISPLAY_LVGL_DRAW_BUF_LINES * sizeof(lv_color_t);

    ESP_LOGI(TAG, "Draw buffers: %d lines, %u bytes each, %u bytes total, %s",
             DISPLAY_LVGL_DRAW_BUF_LINES, (unsigned int)draw_buffer_sz,
             (unsigned int)(draw_buffer_sz * 2), DISPLAY_LVGL_DRAW_BUF_MEM_NAME);

    void *buf1 = heap_caps_aligned_calloc(alignment, 1, draw_buffer_sz, DISPLAY_LVGL_DRAW_BUF_CAPS);
    void *buf2 = heap_caps_aligned_calloc(alignment, 1, draw_buffer_sz, DISPLAY_LVGL_DRAW_BUF_CAPS);

    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "allocate LVGL draw buffers failed");
    lv_display_set_buffers(display, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    return ESP_OK;
}

static esp_err_t display_lvgl_register_panel_callbacks(const display_panel_t *panel, lv_display_t *display)
{
    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = display_lvgl_notify_flush_ready,
#if CONFIG_EXAMPLE_MONITOR_REFRESH_BY_GPIO
        .on_refresh_done = display_board_monitor_refresh_rate,
#endif
    };

    return esp_lcd_dpi_panel_register_event_callbacks(panel->panel_handle, &cbs, display);
}

esp_err_t display_lvgl_init(const display_panel_t *panel, display_lvgl_t *out_lvgl)
{
    ESP_RETURN_ON_FALSE(panel && out_lvgl, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    ESP_LOGI(TAG, "Initialize LVGL");
    lv_init();

    lv_display_t *display = lv_display_create(panel->h_res, panel->v_res);
    ESP_RETURN_ON_FALSE(display, ESP_ERR_NO_MEM, TAG, "create LVGL display failed");

    lv_display_set_user_data(display, panel->panel_handle);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    ESP_RETURN_ON_ERROR(display_lvgl_allocate_buffers(display), TAG, "allocate LVGL draw buffers failed");
    lv_display_set_flush_cb(display, display_lvgl_flush_cb);

#if CONFIG_EXAMPLE_LVGL_PERF_LOG
    lv_display_add_event_cb(display, display_lvgl_perf_event_cb, LV_EVENT_ALL, NULL);
#endif

#if CONFIG_EXAMPLE_USE_DMA2D_COPY_FRAME
    if (esp_efuse_is_flash_encryption_enabled()) {
        lv_display_add_event_cb(display, display_lvgl_rounder_flush_area_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }
#endif

    ESP_RETURN_ON_ERROR(display_lvgl_register_panel_callbacks(panel, display), TAG, "register DPI callbacks failed");

    /* Initialize GT911 touch controller and register LVGL input device */
    lv_indev_t *indev = NULL;
    ESP_RETURN_ON_ERROR(display_touch_init(), TAG, "init touch failed");
    ESP_RETURN_ON_ERROR(display_touch_register_lvgl_indev(display, &indev), TAG, "register touch indev failed");

    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = display_lvgl_increase_tick,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer), TAG, "create LVGL tick timer failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(lvgl_tick_timer, DISPLAY_LVGL_TICK_PERIOD_MS * 1000), TAG, "start LVGL tick timer failed");

    BaseType_t task_created = xTaskCreate(display_lvgl_port_task, "LVGL", DISPLAY_LVGL_TASK_STACK_SIZE, NULL,
                                          DISPLAY_LVGL_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(task_created == pdPASS, ESP_ERR_NO_MEM, TAG, "create LVGL task failed");

    out_lvgl->display = display;
    out_lvgl->indev = indev;
    return ESP_OK;
}

void display_lvgl_lock(void)
{
    _lock_acquire(&s_lvgl_api_lock);
}

void display_lvgl_unlock(void)
{
    _lock_release(&s_lvgl_api_lock);
}
