#include "block_physics.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "block_physics";

/*
 * RGB565 高字节在前（SH8601 面板线路字节序，参考 doc/display-pipeline.md）。
 * 调色板在初始化时完成字节交换，运行时零开销。
 */
#define BLOCK_RGB565(r, g, b)                                                                      \
    ((uint16_t)((((uint16_t)(r)&0xF8) << 8) | (((uint16_t)(g)&0xFC) << 3) | (((uint16_t)(b)&0xF8) >> 3)))
#define BLOCK_RGB565_WIRE(r, g, b)                                                                 \
    ((uint16_t)(((BLOCK_RGB565(r, g, b) & 0x00FFu) << 8) | ((BLOCK_RGB565(r, g, b) & 0xFF00u) >> 8)))

#define BLOCK_POS_CORR_Q8 204          /* 每次迭代穿透修正比例（Q8.8，80%） */
#define BLOCK_SHAKE_DECAY_Q8 250       /* 晃动能量每步衰减（Q8.8，97.7%） */
#define BLOCK_IMU_TIMEOUT_US 500000    /* IMU 失联 0.5s 后回退默认重力 */

/* 方块：轴对齐 AABB，中心坐标与速度均为 Q8.8 定点，质量 = 面积 */
struct Block {
    int32_t x;         /* 中心 X，Q8.8 */
    int32_t y;         /* 中心 Y，Q8.8 */
    int32_t vx;        /* 速度 X，Q8.8 px/step */
    int32_t vy;
    uint8_t w;         /* 尺寸 px */
    uint8_t h;
    uint16_t color;    /* 已交换字节序的 RGB565 */
    uint32_t inv_mass; /* Q16.16 = 65536/(w*h) */
    bool contact;      /* 本步是否与墙/其他方块接触 */
};

struct block_physics {
    block_physics_config_t cfg;
    esp_lcd_panel_handle_t panel;

    Block *blocks;        /* 物理侧状态 */
    Block *snapshot;      /* 物理 -> 渲染快照 */
    Block *render_blocks; /* 渲染任务本地拷贝 */
    uint16_t *frame_buf[3];
    int current_fb;
    uint16_t palette[16];

    SemaphoreHandle_t snapshot_mutex;
    SemaphoreHandle_t imu_mutex;
    volatile bool running;

    TaskHandle_t physics_task;
    TaskHandle_t render_task;

    /* IMU 状态（imu_mutex 保护） */
    bool imu_active;
    uint64_t last_imu_us;
    int32_t lp_x;     /* 低通滤波后加速度（原始 LSB） */
    int32_t lp_y;
    int32_t lp_z;
    int32_t grav_x;   /* 屏幕重力加速度（Q8.8 px/step^2） */
    int32_t grav_y;
    uint32_t shake;   /* 晃动能量（原始 LSB 和，物理侧逐帧衰减） */
    bool imu_effective; /* 最近一步 IMU 是否生效（供 stats） */

    uint64_t last_physics_us;
    uint64_t last_render_us;
    uint64_t last_submit_us;
    float avg_frame_us;
    uint64_t stats_next_log_us;
};

/* 8~10 个高饱和度颜色（Kconfig 可调数量，最多 16） */
static const struct {
    uint8_t r, g, b;
} kPalette[] = {
    {255, 70, 70},   /* 红 */
    {255, 170, 40},  /* 橙 */
    {250, 225, 60},  /* 黄 */
    {90, 230, 90},   /* 绿 */
    {60, 205, 255},  /* 青 */
    {80, 140, 255},  /* 蓝 */
    {185, 100, 255}, /* 紫 */
    {255, 110, 200}, /* 粉 */
    {210, 215, 230}, /* 亮灰 */
};

static void *spiram_calloc(size_t n, size_t size)
{
    void *p = heap_caps_malloc(n * size, MALLOC_CAP_SPIRAM);
    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}

static uint32_t irand(uint32_t range)
{
    return range > 0 ? esp_random() % range : 0;
}

static inline int32_t block_iabs(int32_t v)
{
    return v < 0 ? -v : v;
}

static void init_block(Block *b, int hr, int vr, const uint16_t *palette)
{
    const int min_sz = CONFIG_BLOCK_PHYSICS_BLOCK_MIN_SIZE;
    const int max_sz = CONFIG_BLOCK_PHYSICS_BLOCK_MAX_SIZE;
    b->w = (uint8_t)(min_sz + irand((uint32_t)(max_sz - min_sz + 1)));
    b->h = (uint8_t)(min_sz + irand((uint32_t)(max_sz - min_sz + 1)));
    b->color = palette[irand(CONFIG_BLOCK_PHYSICS_COLOR_COUNT)];
    b->inv_mass = (1u << 16) / ((uint32_t)b->w * b->h);
    const int half_w = b->w / 2;
    const int half_h = b->h / 2;
    b->x = ((int32_t)irand((uint32_t)(hr - b->w)) + half_w) << 8;
    b->y = ((int32_t)irand((uint32_t)(vr - b->h)) + half_h) << 8;
    b->vx = 0;
    b->vy = 0;
    b->contact = false;
}

/* 随机分布方块，尽量避免初始重叠（重试上限内） */
static void init_blocks(block_physics_t *self)
{
    for (int i = 0; i < self->cfg.block_count; i++) {
        Block *b = &self->blocks[i];
        for (int attempt = 0; attempt < 128; attempt++) {
            init_block(b, self->cfg.h_res, self->cfg.v_res, self->palette);
            bool overlap = false;
            for (int j = 0; j < i; j++) {
                const Block &o = self->blocks[j];
                const int32_t dx = block_iabs(b->x - o.x) >> 8;
                const int32_t dy = block_iabs(b->y - o.y) >> 8;
                if (dx < ((int32_t)b->w + o.w + 2) / 2 && dy < ((int32_t)b->h + o.h + 2) / 2) {
                    overlap = true;
                    break;
                }
            }
            if (!overlap) {
                break;
            }
        }
    }
}

/*
 * 一对 AABB 的碰撞求解（参考 Box2D Lite 思路）：
 * 沿穿透最小的轴做位置修正（按质量分摊，每次迭代修正 80%），
 * 相对速度沿法向按恢复系数反弹，切向施加摩擦。
 */
static void solve_pair(Block &a, Block &b)
{
    const int32_t hw_a = (int32_t)a.w << 7;
    const int32_t hw_b = (int32_t)b.w << 7;
    const int32_t hh_a = (int32_t)a.h << 7;
    const int32_t hh_b = (int32_t)b.h << 7;
    const int32_t dx = b.x - a.x;
    const int32_t dy = b.y - a.y;
    const int32_t ox = (hw_a + hw_b) - block_iabs(dx);
    const int32_t oy = (hh_a + hh_b) - block_iabs(dy);
    if (ox <= 0 || oy <= 0) {
        return;
    }

    const int32_t total = (int32_t)(a.inv_mass + b.inv_mass);
    const int32_t rest_k = 256 + CONFIG_BLOCK_PHYSICS_RESTITUTION_Q8;
    const int32_t fr = CONFIG_BLOCK_PHYSICS_FRICTION_Q8;

    if (ox < oy) {
        /* X 轴法向 */
        const int32_t sign = dx >= 0 ? 1 : -1;
        a.x -= (sign * ((ox * (int32_t)a.inv_mass) / total) * BLOCK_POS_CORR_Q8) >> 8;
        b.x += (sign * ((ox * (int32_t)b.inv_mass) / total) * BLOCK_POS_CORR_Q8) >> 8;
        const int32_t vn = sign * (a.vx - b.vx);
        if (vn > 0) {
            const int32_t dv_a = (((vn * rest_k) >> 8) * (int32_t)a.inv_mass) / total;
            const int32_t dv_b = (((vn * rest_k) >> 8) * (int32_t)b.inv_mass) / total;
            a.vx -= sign * dv_a;
            b.vx += sign * dv_b;
        }
        const int32_t vt = sign * (a.vy - b.vy);
        if (vt != 0) {
            const int32_t dv_a = (((vt * fr) >> 8) * (int32_t)a.inv_mass) / total;
            const int32_t dv_b = (((vt * fr) >> 8) * (int32_t)b.inv_mass) / total;
            a.vy -= sign * dv_a;
            b.vy += sign * dv_b;
        }
    } else {
        /* Y 轴法向 */
        const int32_t sign = dy >= 0 ? 1 : -1;
        a.y -= (sign * ((oy * (int32_t)a.inv_mass) / total) * BLOCK_POS_CORR_Q8) >> 8;
        b.y += (sign * ((oy * (int32_t)b.inv_mass) / total) * BLOCK_POS_CORR_Q8) >> 8;
        const int32_t vn = sign * (a.vy - b.vy);
        if (vn > 0) {
            const int32_t dv_a = (((vn * rest_k) >> 8) * (int32_t)a.inv_mass) / total;
            const int32_t dv_b = (((vn * rest_k) >> 8) * (int32_t)b.inv_mass) / total;
            a.vy -= sign * dv_a;
            b.vy += sign * dv_b;
        }
        const int32_t vt = sign * (a.vx - b.vx);
        if (vt != 0) {
            const int32_t dv_a = (((vt * fr) >> 8) * (int32_t)a.inv_mass) / total;
            const int32_t dv_b = (((vt * fr) >> 8) * (int32_t)b.inv_mass) / total;
            a.vx -= sign * dv_a;
            b.vx += sign * dv_b;
        }
    }
    a.contact = true;
    b.contact = true;
}

static void step_physics(block_physics_t *self)
{
    const int hr = self->cfg.h_res;
    const int vr = self->cfg.v_res;
    const int32_t grav_default = CONFIG_BLOCK_PHYSICS_GRAVITY_ACC_Q8;

    int32_t grav_x = 0;
    int32_t grav_y = grav_default;
    uint32_t shake = 0;

    xSemaphoreTake(self->imu_mutex, portMAX_DELAY);
    const bool imu_ok = self->imu_active &&
                        (esp_timer_get_time() - self->last_imu_us < BLOCK_IMU_TIMEOUT_US);
    if (imu_ok) {
        grav_x = self->grav_x;
        grav_y = self->grav_y;
        shake = self->shake;
        self->shake = (self->shake * BLOCK_SHAKE_DECAY_Q8) >> 8;
    } else {
        self->imu_active = false;
    }
    xSemaphoreGive(self->imu_mutex);
    self->imu_effective = imu_ok;

    const int32_t shake_thr = CONFIG_BLOCK_PHYSICS_SHAKE_THRESHOLD;
    const int32_t shake_gain = CONFIG_BLOCK_PHYSICS_SHAKE_GAIN_Q8;
    const int32_t damping = CONFIG_BLOCK_PHYSICS_DAMPING_Q8;
    const int32_t rest_q = CONFIG_BLOCK_PHYSICS_RESTITUTION_Q8;
    const int32_t inv_friction = 256 - CONFIG_BLOCK_PHYSICS_FRICTION_Q8;
    const int count = self->cfg.block_count;
    Block *blocks = self->blocks;

    /* 积分：重力 + 晃动冲量 + 阻尼 + 位移 + 墙边界 */
    for (int i = 0; i < count; i++) {
        Block &b = blocks[i];
        b.contact = false;
        b.vx += grav_x;
        b.vy += grav_y;
        if (shake > (uint32_t)shake_thr) {
            const int32_t raw = (int32_t)(shake - (uint32_t)shake_thr);
            int32_t imp = (raw * shake_gain) >> 8;
            if (imp > CONFIG_BLOCK_PHYSICS_MAX_VEL_Q8 / 2) {
                imp = CONFIG_BLOCK_PHYSICS_MAX_VEL_Q8 / 2;
            }
            b.vx += (int32_t)irand((uint32_t)(2 * imp + 1)) - imp;
            b.vy += (int32_t)irand((uint32_t)(2 * imp + 1)) - imp;
        }
        b.vx = (b.vx * damping) >> 8;
        b.vy = (b.vy * damping) >> 8;
        if (b.vx > CONFIG_BLOCK_PHYSICS_MAX_VEL_Q8) {
            b.vx = CONFIG_BLOCK_PHYSICS_MAX_VEL_Q8;
        } else if (b.vx < -CONFIG_BLOCK_PHYSICS_MAX_VEL_Q8) {
            b.vx = -CONFIG_BLOCK_PHYSICS_MAX_VEL_Q8;
        }
        if (b.vy > CONFIG_BLOCK_PHYSICS_MAX_VEL_Q8) {
            b.vy = CONFIG_BLOCK_PHYSICS_MAX_VEL_Q8;
        } else if (b.vy < -CONFIG_BLOCK_PHYSICS_MAX_VEL_Q8) {
            b.vy = -CONFIG_BLOCK_PHYSICS_MAX_VEL_Q8;
        }
        b.x += b.vx;
        b.y += b.vy;

        const int32_t hw = (int32_t)b.w << 7;
        const int32_t hh = (int32_t)b.h << 7;
        const int32_t min_x = hw;
        const int32_t max_x = ((int32_t)hr << 8) - hw;
        const int32_t min_y = hh;
        const int32_t max_y = ((int32_t)vr << 8) - hh;
        if (b.x < min_x) {
            b.x = min_x;
            if (b.vx < 0) {
                b.vx = -(b.vx * rest_q) >> 8;
            }
            b.vy = (b.vy * inv_friction) >> 8;
            b.contact = true;
        } else if (b.x > max_x) {
            b.x = max_x;
            if (b.vx > 0) {
                b.vx = -(b.vx * rest_q) >> 8;
            }
            b.vy = (b.vy * inv_friction) >> 8;
            b.contact = true;
        }
        if (b.y < min_y) {
            b.y = min_y;
            if (b.vy < 0) {
                b.vy = -(b.vy * rest_q) >> 8;
            }
            b.vx = (b.vx * inv_friction) >> 8;
            b.contact = true;
        } else if (b.y > max_y) {
            b.y = max_y;
            if (b.vy > 0) {
                b.vy = -(b.vy * rest_q) >> 8;
            }
            b.vx = (b.vx * inv_friction) >> 8;
            b.contact = true;
        }
    }

    /* 碰撞迭代：穿透分离 + 冲量反弹 + 摩擦 */
    for (int it = 0; it < self->cfg.iterations; it++) {
        for (int i = 0; i < count; i++) {
            for (int j = i + 1; j < count; j++) {
                solve_pair(blocks[i], blocks[j]);
            }
        }
    }
}

static void draw_blocks(block_physics_t *self, const Block *blocks)
{
    const int hr = self->cfg.h_res;
    const int vr = self->cfg.v_res;
    uint16_t *fb = self->frame_buf[self->current_fb];
    memset(fb, 0, (size_t)hr * vr * sizeof(uint16_t));

    for (int i = 0; i < self->cfg.block_count; i++) {
        const Block &b = blocks[i];
        const int x0 = (int)((b.x - ((int32_t)b.w << 7)) >> 8);
        const int y0 = (int)((b.y - ((int32_t)b.h << 7)) >> 8);
        const int x1 = x0 + b.w;
        const int y1 = y0 + b.h;
        if (x1 <= 0 || y1 <= 0 || x0 >= hr || y0 >= vr) {
            continue;
        }
        const int cx0 = x0 < 0 ? 0 : x0;
        const int cx1 = x1 > hr ? hr : x1;
        const int cy0 = y0 < 0 ? 0 : y0;
        const int cy1 = y1 > vr ? vr : y1;
        for (int y = cy0; y < cy1; y++) {
            uint16_t *row = fb + (size_t)y * hr + cx0;
            for (int x = cx0; x < cx1; x++) {
                *row++ = b.color;
            }
        }
    }
}

static void physics_task(void *arg)
{
    block_physics_t *self = (block_physics_t *)arg;
    const TickType_t period = pdMS_TO_TICKS(1000 / self->cfg.physics_hz);
    TickType_t last_wake = xTaskGetTickCount();
    while (self->running) {
        const uint64_t t0 = esp_timer_get_time();
        step_physics(self);
        self->last_physics_us = esp_timer_get_time() - t0;
        xSemaphoreTake(self->snapshot_mutex, portMAX_DELAY);
        memcpy(self->snapshot, self->blocks, (size_t)self->cfg.block_count * sizeof(Block));
        xSemaphoreGive(self->snapshot_mutex);
        xTaskDelayUntil(&last_wake, period);
    }
    /* 任务不自行删除，由 block_physics_stop() 统一回收 */
    vTaskDelay(portMAX_DELAY);
}

static void render_task(void *arg)
{
    block_physics_t *self = (block_physics_t *)arg;
    const TickType_t period = pdMS_TO_TICKS(1000 / self->cfg.render_hz);
    TickType_t last_wake = xTaskGetTickCount();
    uint64_t frame_start = esp_timer_get_time();
    while (self->running) {
        xSemaphoreTake(self->snapshot_mutex, portMAX_DELAY);
        memcpy(self->render_blocks, self->snapshot, (size_t)self->cfg.block_count * sizeof(Block));
        xSemaphoreGive(self->snapshot_mutex);

        uint64_t t0 = esp_timer_get_time();
        draw_blocks(self, self->render_blocks);
        self->last_render_us = esp_timer_get_time() - t0;

        t0 = esp_timer_get_time();
        esp_err_t err = esp_lcd_panel_draw_bitmap(self->panel, 0, 0, self->cfg.h_res,
                                                  self->cfg.v_res, self->frame_buf[self->current_fb]);
        self->last_submit_us = esp_timer_get_time() - t0;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "draw_bitmap failed: %s", esp_err_to_name(err));
        }
        self->current_fb = (self->current_fb + 1) % 3;

        const uint64_t now = esp_timer_get_time();
        const float frame_us = (float)(now - frame_start);
        self->avg_frame_us = self->avg_frame_us * 0.9f + frame_us * 0.1f;
        frame_start = now;

        if (CONFIG_BLOCK_PHYSICS_STATS_INTERVAL_MS > 0 && now >= self->stats_next_log_us) {
            const float avg = self->avg_frame_us > 1.0f ? self->avg_frame_us : 1.0f;
            ESP_LOGI(TAG,
                     "stats: fps=%.1f frame=%.2fms physics=%.2fms render=%.2fms submit=%.2fms blocks=%d imu=%d",
                     1e6f / avg, avg / 1000.0f, (float)self->last_physics_us / 1000.0f,
                     (float)self->last_render_us / 1000.0f, (float)self->last_submit_us / 1000.0f,
                     self->cfg.block_count, self->imu_effective ? 1 : 0);
            self->stats_next_log_us = now + (uint64_t)CONFIG_BLOCK_PHYSICS_STATS_INTERVAL_MS * 1000;
        }
        xTaskDelayUntil(&last_wake, period);
    }
    vTaskDelay(portMAX_DELAY);
}

block_physics_config_t block_physics_default_config(void)
{
    block_physics_config_t cfg;
    cfg.h_res = 368;
    cfg.v_res = 448;
    cfg.block_count = CONFIG_BLOCK_PHYSICS_BLOCK_COUNT;
    cfg.physics_hz = CONFIG_BLOCK_PHYSICS_PHYSICS_HZ;
    cfg.render_hz = CONFIG_BLOCK_PHYSICS_RENDER_HZ;
    cfg.iterations = CONFIG_BLOCK_PHYSICS_ITERATIONS;
    return cfg;
}

esp_err_t block_physics_create(const block_physics_config_t *config,
                               esp_lcd_panel_handle_t panel,
                               block_physics_handle_t *out_handle)
{
    if (config == NULL || panel == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->h_res < 2 || config->v_res < 2 || config->block_count < 1 ||
        config->block_count > 128 || config->physics_hz < 1 || config->physics_hz > 240 ||
        config->render_hz < 1 || config->render_hz > 120 ||
        config->iterations < 1 || config->iterations > 8) {
        return ESP_ERR_INVALID_ARG;
    }

    const int color_count = CONFIG_BLOCK_PHYSICS_COLOR_COUNT;
    block_physics_t *self = (block_physics_t *)calloc(1, sizeof(block_physics_t));
    if (self == NULL) {
        return ESP_ERR_NO_MEM;
    }
    self->cfg = *config;
    self->panel = panel;
    self->avg_frame_us = 1e6f / (float)config->render_hz;

    const size_t fb_bytes = (size_t)config->h_res * config->v_res * sizeof(uint16_t);
    self->blocks = (Block *)spiram_calloc((size_t)config->block_count, sizeof(Block));
    self->snapshot = (Block *)spiram_calloc((size_t)config->block_count, sizeof(Block));
    self->render_blocks = (Block *)spiram_calloc((size_t)config->block_count, sizeof(Block));
    self->frame_buf[0] = (uint16_t *)spiram_calloc(1, fb_bytes);
    self->frame_buf[1] = (uint16_t *)spiram_calloc(1, fb_bytes);
    self->frame_buf[2] = (uint16_t *)spiram_calloc(1, fb_bytes);
    if (self->blocks == NULL || self->snapshot == NULL || self->render_blocks == NULL ||
        self->frame_buf[0] == NULL || self->frame_buf[1] == NULL || self->frame_buf[2] == NULL) {
        goto fail;
    }

    self->snapshot_mutex = xSemaphoreCreateMutex();
    self->imu_mutex = xSemaphoreCreateMutex();
    if (self->snapshot_mutex == NULL || self->imu_mutex == NULL) {
        goto fail;
    }

    for (int i = 0; i < color_count; i++) {
        self->palette[i] = BLOCK_RGB565_WIRE(kPalette[i].r, kPalette[i].g, kPalette[i].b);
    }
    init_blocks(self);

    *out_handle = self;
    return ESP_OK;

fail:
    if (self->snapshot_mutex) {
        vSemaphoreDelete(self->snapshot_mutex);
    }
    if (self->imu_mutex) {
        vSemaphoreDelete(self->imu_mutex);
    }
    heap_caps_free(self->blocks);
    heap_caps_free(self->snapshot);
    heap_caps_free(self->render_blocks);
    heap_caps_free(self->frame_buf[0]);
    heap_caps_free(self->frame_buf[1]);
    heap_caps_free(self->frame_buf[2]);
    free(self);
    return ESP_ERR_NO_MEM;
}

esp_err_t block_physics_start(block_physics_handle_t handle)
{
    if (handle == NULL || handle->running) {
        return ESP_ERR_INVALID_STATE;
    }

    handle->running = true;
    handle->stats_next_log_us = esp_timer_get_time() +
                                (uint64_t)CONFIG_BLOCK_PHYSICS_STATS_INTERVAL_MS * 1000;
    if (xTaskCreatePinnedToCore(physics_task, "block_phys", 4096, handle, 5, &handle->physics_task, 0) != pdPASS) {
        handle->running = false;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(render_task, "block_render", 4096, handle, 6, &handle->render_task, 1) != pdPASS) {
        handle->running = false;
        vTaskDelete(handle->physics_task);
        handle->physics_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "block physics started (%d blocks, physics %d Hz, render %d Hz, %d iter)",
             handle->cfg.block_count, handle->cfg.physics_hz, handle->cfg.render_hz,
             handle->cfg.iterations);
    return ESP_OK;
}

esp_err_t block_physics_stop(block_physics_handle_t handle)
{
    if (handle == NULL || !handle->running) {
        return ESP_ERR_INVALID_STATE;
    }
    handle->running = false;
    vTaskDelay(pdMS_TO_TICKS(300));
    if (handle->physics_task) {
        vTaskDelete(handle->physics_task);
        handle->physics_task = NULL;
    }
    if (handle->render_task) {
        vTaskDelete(handle->render_task);
        handle->render_task = NULL;
    }
    return ESP_OK;
}

void block_physics_imu_update(block_physics_handle_t handle,
                              int16_t ax, int16_t ay, int16_t az,
                              int16_t gx, int16_t gy, int16_t gz)
{
    if (handle == NULL) {
        return;
    }
    int32_t axs = ax;
    int32_t ays = ay;
#if CONFIG_BLOCK_PHYSICS_IMU_SWAP_XY
    const int32_t t = axs;
    axs = ays;
    ays = t;
#endif
#if CONFIG_BLOCK_PHYSICS_IMU_INVERT_X
    axs = -axs;
#endif
#if CONFIG_BLOCK_PHYSICS_IMU_INVERT_Y
    ays = -ays;
#endif

    const int32_t alpha = CONFIG_BLOCK_PHYSICS_ACCEL_LP_ALPHA;
    const int32_t g_q8 = CONFIG_BLOCK_PHYSICS_GRAVITY_ACC_Q8;
    xSemaphoreTake(handle->imu_mutex, portMAX_DELAY);
    handle->lp_x += ((axs - handle->lp_x) * alpha) >> 8;
    handle->lp_y += ((ays - handle->lp_y) * alpha) >> 8;
    handle->lp_z += (((int32_t)az - handle->lp_z) * alpha) >> 8;
    /* 16384 LSB = 1g（±2g 量程），左移 14 位后满量程刚好等于 g_q8 */
    handle->grav_x = (handle->lp_x * g_q8) >> 14;
    handle->grav_y = (handle->lp_y * g_q8) >> 14;
    const int32_t hpx = axs - handle->lp_x;
    const int32_t hpy = ays - handle->lp_y;
    const int32_t hpz = (int32_t)az - handle->lp_z;
    const int32_t energy = block_iabs(hpx) + block_iabs(hpy) + block_iabs(hpz) +
                           block_iabs(gx) + block_iabs(gy) + block_iabs(gz);
    if ((uint32_t)energy > handle->shake) {
        handle->shake = (uint32_t)energy;
    }
    handle->imu_active = true;
    handle->last_imu_us = esp_timer_get_time();
    xSemaphoreGive(handle->imu_mutex);
}

esp_err_t block_physics_get_stats(block_physics_handle_t handle, block_physics_stats_t *stats)
{
    if (handle == NULL || stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const float avg = handle->avg_frame_us > 1.0f ? handle->avg_frame_us : 1.0f;
    stats->fps = 1e6f / avg;
    stats->avg_frame_us = handle->avg_frame_us;
    stats->physics_us = (float)handle->last_physics_us;
    stats->render_us = (float)handle->last_render_us;
    stats->submit_us = (float)handle->last_submit_us;
    stats->imu_active = handle->imu_effective;
    return ESP_OK;
}
