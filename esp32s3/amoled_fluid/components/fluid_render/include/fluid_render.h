#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int h_res;      /* 屏幕水平分辨率（如 368） */
    int v_res;      /* 屏幕垂直分辨率（如 448） */
    int sim_w;      /* 模拟网格宽（如 92） */
    int sim_h;      /* 模拟网格高（如 112） */
    int sim_hz;     /* 模拟/渲染目标帧率（如 30） */
    int blob_count; /* 液滴数量（如 6） */
} fluid_render_config_t;

typedef struct fluid_render fluid_render_t;
typedef fluid_render_t *fluid_render_handle_t;

/** 基于组件 Kconfig 默认值构建配置；h_res/v_res 调用方可覆盖为实际面板值 */
fluid_render_config_t fluid_render_default_config(void);

/**
 * 创建流体渲染器。只依赖面板句柄与分辨率，不访问硬件寄存器。
 * 内部为模拟/渲染分配 PSRAM 缓冲，创建后需调用 fluid_render_start 启动任务。
 */
esp_err_t fluid_render_create(const fluid_render_config_t *config,
                              esp_lcd_panel_handle_t panel,
                              fluid_render_handle_t *out_handle);

esp_err_t fluid_render_start(fluid_render_handle_t handle);
esp_err_t fluid_render_stop(fluid_render_handle_t handle);

/** 触摸事件注入（线程安全）：pressed=1 按下、0 抬起；x/y 为屏幕坐标 */
void fluid_render_touch_event(fluid_render_handle_t handle, bool pressed, uint16_t x, uint16_t y);

typedef struct {
    float fps;          /* 最近平均帧率 */
    float avg_frame_us; /* 平均帧周期（us） */
    float sim_us;       /* 最近一次模拟耗时（us） */
    float upscale_us;   /* 最近一次放大渲染耗时（us） */
    float submit_us;    /* 最近一次面板提交耗时（us） */
} fluid_render_stats_t;

esp_err_t fluid_render_get_stats(fluid_render_handle_t handle, fluid_render_stats_t *stats);

#ifdef __cplusplus
}
#endif
