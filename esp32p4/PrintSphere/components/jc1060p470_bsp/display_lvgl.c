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
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_efuse.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#if CONFIG_EXAMPLE_LVGL_PORT_ESP_ADAPTER
#include "esp_lv_adapter.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display_lvgl";
#if CONFIG_EXAMPLE_LVGL_PORT_CUSTOM_LEGACY
static _lock_t s_lvgl_api_lock;
#endif

extern void example_lvgl_demo_ui(lv_display_t *disp, void (*progress_cb)(const char *stage));

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
    if (report_elapsed_ms < 5000) {
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

#if CONFIG_EXAMPLE_LVGL_PORT_ESP_ADAPTER
static const char *display_lvgl_adapter_tear_mode_name(esp_lv_adapter_tear_avoid_mode_t mode)
{
    switch (mode) {
    case ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT:
        return "DOUBLE_DIRECT";
    case ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL:
        return "TRIPLE_PARTIAL";
    case ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_FULL:
        return "DOUBLE_FULL";
    default:
        return "OTHER";
    }
}

static esp_err_t display_lvgl_init_adapter(const display_panel_t *panel,
                                           const esp_lv_adapter_config_t *requested_adapter_cfg,
                                           esp_lv_adapter_rotation_t rotation,
                                           esp_lv_adapter_tear_avoid_mode_t tear_mode,
                                           display_lvgl_t *out_lvgl)
{
    ESP_LOGI(TAG, "Initialize LVGL via esp_lvgl_adapter");
    ESP_LOGI(TAG, "LVGL v%d.%d.%d lvgl_builtin_ppa=%d DMA2D_hook=%d adapter_ppa=%d tear=%s num_fbs=%u lane=%dMbps dpi=%dMHz",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
#if LV_USE_PPA
             1,
#else
             0,
#endif
             CONFIG_EXAMPLE_USE_DMA2D_COPY_FRAME,
             CONFIG_EXAMPLE_LVGL_ADAPTER_ENABLE_PPA_ACCEL,
             display_lvgl_adapter_tear_mode_name(tear_mode),
             (unsigned int)panel->num_fbs,
             DISPLAY_MIPI_DSI_LANE_BITRATE_MBPS,
             DISPLAY_MIPI_DSI_DPI_CLK_MHZ);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    if (requested_adapter_cfg != NULL) {
        adapter_cfg = *requested_adapter_cfg;
    }
    esp_err_t ret = esp_lv_adapter_init(&adapter_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init esp_lvgl_adapter failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lv_adapter_display_config_t disp_cfg = ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
        panel->panel_handle,
        panel->panel_io_handle,
        panel->h_res,
        panel->v_res,
        rotation);
    disp_cfg.tear_avoid_mode = tear_mode;
    disp_cfg.profile.use_psram = true;
    disp_cfg.profile.enable_ppa_accel = CONFIG_EXAMPLE_LVGL_ADAPTER_ENABLE_PPA_ACCEL;
    disp_cfg.profile.buffer_height = DISPLAY_LVGL_DRAW_BUF_LINES;

    lv_display_t *display = esp_lv_adapter_register_display(&disp_cfg);
    if (display == NULL) {
        ESP_LOGE(TAG, "register adapter display failed");
        ret = ESP_ERR_NO_MEM;
        goto cleanup_adapter;
    }

#if CONFIG_EXAMPLE_LVGL_PERF_LOG
    lv_display_add_event_cb(display, display_lvgl_perf_event_cb, LV_EVENT_ALL, NULL);
#endif

    ret = display_touch_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init touch failed: %s", esp_err_to_name(ret));
        goto cleanup_display;
    }
    lv_indev_t *indev = NULL;
    ret = display_touch_register_lvgl_indev(display, &indev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register touch indev failed: %s", esp_err_to_name(ret));
        goto cleanup_touch;
    }

    ret = esp_lv_adapter_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start esp_lvgl_adapter failed: %s", esp_err_to_name(ret));
        goto cleanup_indev;
    }

    out_lvgl->display = display;
    out_lvgl->indev = indev;
    return ESP_OK;

cleanup_indev:
    (void)display_touch_unregister_lvgl_indev();
cleanup_touch:
    (void)display_touch_deinit();
cleanup_display:
    (void)esp_lv_adapter_unregister_display(display);
cleanup_adapter:
    (void)esp_lv_adapter_deinit();
    return ret;
}
#endif

#if CONFIG_EXAMPLE_LVGL_PORT_CUSTOM_LEGACY
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
    esp_cache_msync(px_map, DISPLAY_PANEL_H_RES * DISPLAY_PANEL_V_RES * sizeof(lv_color_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
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
    ESP_LOGI(TAG, "LVGL task stack high water mark: %u bytes",
             (unsigned int)uxTaskGetStackHighWaterMark(NULL));

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

static esp_err_t display_lvgl_allocate_buffers(lv_display_t *display, esp_lcd_panel_handle_t panel_handle)
{
    void *fb0 = NULL;
    void *fb1 = NULL;
    const uint32_t num_fbs = DISPLAY_MIPI_DSI_FRAME_BUFFER_NUM;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(panel_handle, num_fbs, &fb0, &fb1),
                        TAG, "get DPI frame buffers failed");

    const size_t fb_size = DISPLAY_PANEL_H_RES * DISPLAY_PANEL_V_RES * sizeof(lv_color_t);
    ESP_LOGI(TAG, "DIRECT mode: %u frame buffers, %u bytes each, fb0=%p fb1=%p",
             (unsigned int)num_fbs, (unsigned int)fb_size, fb0, fb1);

    lv_display_set_buffers(display, fb0, fb1, fb_size, LV_DISPLAY_RENDER_MODE_DIRECT);
    return ESP_OK;
}

static esp_err_t display_lvgl_register_panel_callbacks(const display_panel_t *panel, lv_display_t *display)
{
    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = display_lvgl_notify_flush_ready,
    };

    return esp_lcd_dpi_panel_register_event_callbacks(panel->panel_handle, &cbs, display);
}
#endif

esp_err_t display_lvgl_init(const display_panel_t *panel,
                            const esp_lv_adapter_config_t *adapter_cfg,
                            esp_lv_adapter_rotation_t rotation,
                            esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode,
                            display_lvgl_t *out_lvgl)
{
    ESP_RETURN_ON_FALSE(panel && out_lvgl, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

#if CONFIG_EXAMPLE_LVGL_PORT_ESP_ADAPTER
    return display_lvgl_init_adapter(panel, adapter_cfg, rotation, tear_avoid_mode, out_lvgl);
#else
    ESP_LOGI(TAG, "Initialize LVGL");
    ESP_LOGI(TAG, "LVGL v%d.%d.%d port=legacy render=DIRECT lvgl_builtin_ppa=%d DMA2D_hook=%d num_fbs=%u lane=%dMbps dpi=%dMHz",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
#if LV_USE_PPA
             1,
#else
             0,
#endif
             CONFIG_EXAMPLE_USE_DMA2D_COPY_FRAME,
             (unsigned int)panel->num_fbs,
             DISPLAY_MIPI_DSI_LANE_BITRATE_MBPS,
             DISPLAY_MIPI_DSI_DPI_CLK_MHZ);
    lv_init();

    lv_display_t *display = lv_display_create(panel->h_res, panel->v_res);
    ESP_RETURN_ON_FALSE(display, ESP_ERR_NO_MEM, TAG, "create LVGL display failed");

    lv_display_set_user_data(display, panel->panel_handle);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    ESP_RETURN_ON_ERROR(display_lvgl_allocate_buffers(display, panel->panel_handle), TAG, "allocate LVGL draw buffers failed");
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
#endif
}

esp_err_t display_lvgl_deinit(display_lvgl_t *lvgl)
{
    ESP_RETURN_ON_FALSE(lvgl != NULL, ESP_ERR_INVALID_ARG, TAG, "lvgl is NULL");
#if CONFIG_EXAMPLE_LVGL_PORT_ESP_ADAPTER
    esp_err_t first_err = display_touch_unregister_lvgl_indev();
    if (lvgl->display != NULL) {
        esp_err_t ret = esp_lv_adapter_unregister_display(lvgl->display);
        if (first_err == ESP_OK) first_err = ret;
    }
    esp_err_t ret = esp_lv_adapter_deinit();
    if (first_err == ESP_OK) first_err = ret;
    ret = display_touch_deinit();
    if (first_err == ESP_OK) first_err = ret;
    *lvgl = (display_lvgl_t) {0};
    return first_err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t display_lvgl_lock(int32_t timeout_ms)
{
#if CONFIG_EXAMPLE_LVGL_PORT_ESP_ADAPTER
    return esp_lv_adapter_lock(timeout_ms);
#else
    (void)timeout_ms;
    _lock_acquire(&s_lvgl_api_lock);
    return ESP_OK;
#endif
}

void display_lvgl_unlock(void)
{
#if CONFIG_EXAMPLE_LVGL_PORT_ESP_ADAPTER
    esp_lv_adapter_unlock();
#else
    _lock_release(&s_lvgl_api_lock);
#endif
}
