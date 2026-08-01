#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int h_res;        /* 屏幕水平分辨率（如 368） */
    int v_res;        /* 屏幕垂直分辨率（如 448） */
    int block_count;  /* 方块数量（40~60） */
    int physics_hz;   /* 物理求解频率（如 60） */
    int render_hz;    /* 渲染目标帧率（如 30） */
    int iterations;   /* 每步碰撞迭代次数（2~3） */
} block_physics_config_t;

typedef struct block_physics block_physics_t;
typedef block_physics_t *block_physics_handle_t;

/** 基于组件 Kconfig 默认值构建配置；h_res/v_res 调用方可覆盖为实际面板值 */
block_physics_config_t block_physics_default_config(void);

/**
 * 创建方块物理引擎。只依赖面板句柄与分辨率，不访问硬件寄存器。
 * 内部为物理/渲染分配 PSRAM 缓冲，创建后需调用 block_physics_start 启动任务。
 */
esp_err_t block_physics_create(const block_physics_config_t *config,
                               esp_lcd_panel_handle_t panel,
                               block_physics_handle_t *out_handle);

esp_err_t block_physics_start(block_physics_handle_t handle);
esp_err_t block_physics_stop(block_physics_handle_t handle);

/** IMU 原始读数注入（线程安全），由 main 的 IMU 采样任务 ~100Hz 调用 */
void block_physics_imu_update(block_physics_handle_t handle,
                              int16_t ax, int16_t ay, int16_t az,
                              int16_t gx, int16_t gy, int16_t gz);

typedef struct {
    float fps;          /* 最近平均渲染帧率 */
    float avg_frame_us; /* 平均帧周期（us） */
    float physics_us;   /* 最近一次物理求解耗时（us） */
    float render_us;    /* 最近一次绘制耗时（us） */
    float submit_us;    /* 最近一次面板提交耗时（us） */
    bool imu_active;    /* IMU 是否生效（失联时回退默认重力） */
} block_physics_stats_t;

esp_err_t block_physics_get_stats(block_physics_handle_t handle, block_physics_stats_t *stats);

#ifdef __cplusplus
}
#endif
