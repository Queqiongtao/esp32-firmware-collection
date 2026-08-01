#include "fluid_render.h"

#include <math.h>
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

static const char *TAG = "fluid_render";

/*
 * RGB565 颜色封装（SH8601 高字节在前，参考 companion_display/firmware/esp32s3
 * docs/display-color-format.md 实测结论）。所有绘制颜色在编译期/初始化时交换
 * 字节，直接得到面板线路字节序，零运行时开销。
 */
#define FLUID_RGB565(r, g, b)                                                                      \
    ((uint16_t)((((uint16_t)(r)&0xF8) << 8) | (((uint16_t)(g)&0xFC) << 3) | (((uint16_t)(b)&0xF8) >> 3)))
#define FLUID_RGB565_WIRE(r, g, b)                                                                 \
    ((uint16_t)(((FLUID_RGB565(r, g, b) & 0x00FFu) << 8) | ((FLUID_RGB565(r, g, b) & 0xFF00u) >> 8)))

#define FLUID_SMOOTH_LUT_SIZE 256  /* 平滑核查表：(1 - d2/r2)^2，0..255 */
#define FLUID_SOFT_SAT_K 64        /* 软饱和常数：v = 255 * s / (s + K) */
#define FLUID_MARGIN_FACTOR 4      /* 势能影响范围 = 4 * 基础半径 + 4 */
#define FLUID_TOUCH_LERP 90        /* 绑定液滴向手指移动速度，/256 */
#define FLUID_TOUCH_RADIUS_X 160   /* 触摸时液滴半径倍率，/100 */
#define FLUID_BREATH_AMP 36        /* 呼吸幅度，/256 */
#define FLUID_MAX_VEL 110          /* 漂移速度上限（8.8 定点） */
#define FLUID_RADIUS_LERP 48       /* 半径平滑速度，/256 */
#define FLUID_RELEASE_HOLD 10      /* 抬起后保持高亮帧数 */
#define FLUID_RELEASE_FADE 15      /* 抬起后衰减帧数 */

/* ---- 真实感参数（全部定点） ---- */
#define FLUID_SIZE_VARY_MIN 65     /* 液滴尺寸变化范围下限 /100 */
#define FLUID_SIZE_VARY_MAX 135    /* 液滴尺寸变化范围上限 /100 */
#define FLUID_DAMPING_Q 255        /* 速度阻尼 /256（每帧 *0.996） */
#define FLUID_JITTER_Q 3           /* 随机扰动幅度（8.8 定点） */
#define FLUID_MIN_SPEED_Q 12       /* 最低漂移速度（8.8 定点） */
#define FLUID_GLINT_R2_FACTOR 16   /* 高光半径平方占主液滴百分比 */
#define FLUID_GLINT_OX_FACTOR 30   /* 高光偏移（半径百分比，左上光） */
#define FLUID_GLINT_OY_FACTOR 35
#define FLUID_GLINT_ADD_SHIFT 2    /* 高光叠加：field += spec >> 2 */
#define FLUID_CREASE_DARK_Q 77     /* 融合处变暗强度 /256 */
#define FLUID_COHESION_RANGE_Q 160 /* 凝聚力作用范围（半径倍数 /100） */
#define FLUID_COHESION_NEAR_Q 110  /* 近距排斥边界（半径倍数 /100） */
#define FLUID_COHESION_ATTRACT_Q 4  /* 吸引力强度（8.8 定点） */
#define FLUID_COHESION_REPEL_Q 26   /* 排斥力强度（8.8 定点） */
#define FLUID_WALL_K 48             /* 墙排斥强度（8.8 定点） */
#define FLUID_CENTER_K 96           /* 向心力强度（/65536）：静置时把液滴拉回屏幕中央 */

struct Blob {
    int32_t x;             /* 8.8 定点网格坐标 */
    int32_t y;
    int32_t vx;            /* 8.8 定点速度（每帧） */
    int32_t vy;
    int32_t r2_base;       /* 基础半径平方（每液滴尺寸不同） */
    int32_t r2_target;     /* 目标半径平方 */
    int32_t r2_now;        /* 当前半径平方（平滑过渡） */
    int32_t ux;            /* 运动方向单位向量（8.8） */
    int32_t uy;
    int32_t stretch_q;     /* 沿运动方向拉伸系数（8.8，256=圆） */
    int32_t inv_stretch_q; /* 拉伸倒数（8.8） */
    int32_t glint_ox;      /* 高光偏移（网格整数，左上光） */
    int32_t glint_oy;
    int32_t r2_glint;      /* 高光半径平方 */
    uint8_t phase;         /* 呼吸相位 */
    uint8_t release_hold;
    uint8_t release_fade;
    bool touched;
};

struct fluid_render {
    fluid_render_config_t cfg;
    esp_lcd_panel_handle_t panel;

    uint8_t *field;        /* 模拟势能场 sim_w*sim_h（后期处理后为着色值） */
    uint8_t *field_local;  /* 渲染任务本地快照 */
    uint8_t *spec;         /* 高光场 */
    uint16_t *sum;         /* 未截断的势能累加和（软饱和输入） */
    uint32_t *maxc;        /* 每格主贡献 */
    uint32_t *second_maxc; /* 每格次贡献（用于融合变暗） */
    uint8_t *temp;         /* 水平放大中间缓冲 sim_h*h_res */
    uint16_t *frame_buf[3]; /* 三缓冲：DMA 异步提交时保证写缓冲不与在传缓冲重叠 */
    uint16_t *palette;     /* 256 级调色板（已交换字节序） */
    uint8_t smooth_lut[FLUID_SMOOTH_LUT_SIZE]; /* 平滑核查表（内嵌） */
    uint16_t *recip;       /* 倒数查找表（融合变暗用，免除法） */
    Blob *blobs;

    int current_fb;

    SemaphoreHandle_t sem_field_ready;
    SemaphoreHandle_t sem_field_consumed;
    SemaphoreHandle_t touch_mutex;
    volatile bool running;

    TaskHandle_t sim_task;
    TaskHandle_t render_task;

    bool touch_pressed;
    int touch_x;    /* 屏幕坐标 */
    int touch_y;
    int bound_blob; /* -1 表示未绑定 */

    uint64_t last_sim_us;
    uint64_t last_upscale_us;
    uint64_t last_submit_us;
    float avg_frame_us;
    uint64_t stats_next_log_us;
};

static int16_t s_sin_lut[256];

static void build_sin_lut(void)
{
    for (int i = 0; i < 256; i++) {
        double rad = 2.0 * 3.14159265358979323846 * (double)i / 256.0;
        s_sin_lut[i] = (int16_t)(127.5 * sin(rad));
    }
}

/* 平滑核 f(t) = (1 - t/256)^2 * 255，t 为 d2/r2 的 8.8 定点，t<256 时有效 */
static void build_smooth_lut(uint8_t *sl)
{
    for (int t = 0; t < FLUID_SMOOTH_LUT_SIZE; t++) {
        const int32_t v = 256 - t;
        sl[t] = (uint8_t)((v * v * 255) >> 16);
    }
}

static void build_recip(uint16_t *recip)
{
    for (uint32_t d = 1; d <= 65535u; d++) {
        recip[d] = (uint16_t)(65535u / d);
    }
    recip[0] = 65535;
}

/* 黑 -> 深蓝 -> 青 -> 亮青，仅高光区域接近白；软边缘段足够宽避免色带 */
static void build_palette(uint16_t *palette)
{
    for (int f = 0; f < 256; f++) {
        int r, g, b;
        if (f < 40) {
            r = g = b = 0;
        } else if (f < 90) {
            /* 黑 -> 深蓝（宽软边缘） */
            const int t = (f - 40) * 256 / 50;
            r = 0;
            g = (90 * t) >> 8;
            b = (235 * t) >> 8;
        } else if (f < 170) {
            /* 深蓝 -> 青（液滴主体） */
            const int t = (f - 90) * 256 / 80;
            r = 0;
            g = 90 + ((225 - 90) * t >> 8);
            b = 235 + ((255 - 235) * t >> 8);
        } else if (f < 225) {
            /* 青 -> 亮青（内发光） */
            const int t = (f - 170) * 256 / 55;
            r = 0 + ((120 - 0) * t >> 8);
            g = 225 + ((245 - 225) * t >> 8);
            b = 255;
        } else {
            /* 亮青 -> 近白（只有高光叠加能达到这里） */
            const int t = (f - 225) * 256 / 30;
            r = 120 + ((240 - 120) * t >> 8);
            g = 245 + ((255 - 245) * t >> 8);
            b = 255;
        }
        palette[f] = FLUID_RGB565_WIRE(r, g, b);
    }
}

static void *spiram_calloc(size_t n, size_t size)
{
    void *p = heap_caps_malloc(n * size, MALLOC_CAP_SPIRAM);
    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}

static inline uint32_t isqrt32(uint32_t n)
{
    if (n == 0) {
        return 0;
    }
    uint32_t x = n;
    uint32_t y = (x + 1) >> 1;
    while (y < x) {
        x = y;
        y = (x + n / x) >> 1;
    }
    return x;
}

static inline int32_t blob_r2_eff(const Blob &b)
{
    const int32_t breath = ((int32_t)s_sin_lut[b.phase & 255] * FLUID_BREATH_AMP) / 128;
    return (b.r2_now * (256 + breath)) >> 8;
}

/* 运动参数：单位方向、拉伸、高光（每帧每液滴计算一次） */
static void update_motion_params(Blob &b, int32_t disp_x, int32_t disp_y)
{
    const uint32_t speed = isqrt32((uint32_t)(disp_x * disp_x + disp_y * disp_y));
    if (speed >= 8) {
        b.ux = (disp_x * 256) / (int32_t)speed;
        b.uy = (disp_y * 256) / (int32_t)speed;
        const int32_t smax_q = (CONFIG_FLUID_RENDER_MAX_STRETCH * 256) / 100;
        int32_t s = 256 + ((int32_t)speed * (smax_q - 256)) / 256;
        if (s > smax_q) {
            s = smax_q;
        }
        if (s < 256) {
            s = 256;
        }
        b.stretch_q = s;
        b.inv_stretch_q = (256 * 256) / s;
    } else {
        /* 慢速/静止：给任意单位方向 (256,0)，配合 stretch=256 退化为圆形；
         * 若置 0，拉伸距离 d2 恒为 0，整个 bbox 会全被 255 填满成方形色块 */
        b.ux = 256;
        b.uy = 0;
        b.stretch_q = 256;
        b.inv_stretch_q = 256;
    }
    const int32_t r = (int32_t)isqrt32((uint32_t)b.r2_now);
    b.glint_ox = -(r * FLUID_GLINT_OX_FACTOR) / 100;
    b.glint_oy = -(r * FLUID_GLINT_OY_FACTOR) / 100;
    b.r2_glint = (b.r2_now * FLUID_GLINT_R2_FACTOR) / 100;
}

/*
 * 计算势能场（定点，无浮点、无除法）。核函数为平滑有界核
 * f = (1 - d2/r2)^2：中心 255、边缘 0，内部天然渐变、不产生封顶平台；
 * 每个液滴按拉伸后的椭圆距离贡献，并在左上角贡献离轴高光；
 * 最后按主/次贡献做融合变暗与高光叠加。
 */
static void compute_field(fluid_render_t *self)
{
    const int sw = self->cfg.sim_w;
    const int sh = self->cfg.sim_h;
    const size_t grid = (size_t)sw * sh;
    uint8_t *field = self->field;
    uint8_t *spec = self->spec;
    uint16_t *sum = self->sum;
    uint32_t *maxc = self->maxc;
    uint32_t *second = self->second_maxc;
    memset(field, 0, grid);
    memset(spec, 0, grid);
    memset(sum, 0, grid * sizeof(uint16_t));
    memset(maxc, 0, grid * sizeof(uint32_t));
    memset(second, 0, grid * sizeof(uint32_t));

    const int base_r = CONFIG_FLUID_RENDER_BLOB_RADIUS_CELLS;
    const int margin = FLUID_MARGIN_FACTOR * base_r + 4;
    const uint8_t *sl = self->smooth_lut;

    for (int b = 0; b < self->cfg.blob_count; b++) {
        const Blob &blob = self->blobs[b];
        const int32_t r2 = blob_r2_eff(blob);
        if (r2 <= 0) {
            continue;
        }
        /* 归一化倒数：t = d2 * km >> 8，t<256 表示在液滴内 */
        const int32_t km = (256 * 256) / r2;
        const int32_t kg = blob.r2_glint > 0 ? (256 * 256) / blob.r2_glint : 0;
        const int cx = (int)(blob.x >> 8);
        const int cy = (int)(blob.y >> 8);
        const int x0 = cx > margin ? cx - margin : 0;
        const int x1 = (cx + margin < sw) ? cx + margin : sw - 1;
        const int y0 = cy > margin ? cy - margin : 0;
        const int y1 = (cy + margin < sh) ? cy + margin : sh - 1;

        for (int gy = y0; gy <= y1; gy++) {
            const int dy = gy - cy;
            uint16_t *srow_sum = sum + gy * sw;
            uint8_t *srow = spec + gy * sw;
            uint32_t *mrow = maxc + gy * sw;
            uint32_t *sndrow = second + gy * sw;
            for (int gx = x0; gx <= x1; gx++) {
                const int dx = gx - cx;

                /* 拉伸距离（椭圆化） */
                const int32_t along = (dx * blob.ux + dy * blob.uy) >> 8;
                const int32_t perp = (dx * blob.uy - dy * blob.ux) >> 8;
                const int32_t ax = (along * blob.inv_stretch_q) >> 8;
                const int32_t py = (perp * blob.stretch_q) >> 8;
                const uint32_t d2 = (uint32_t)(ax * ax + py * py);
                const int32_t t = (int32_t)((d2 * (uint32_t)km) >> 8);
                if (t < FLUID_SMOOTH_LUT_SIZE) {
                    const int32_t c = sl[t];
                    srow_sum[gx] = (uint16_t)(srow_sum[gx] + c);
                    if ((uint32_t)c > mrow[gx]) {
                        sndrow[gx] = mrow[gx];
                        mrow[gx] = (uint32_t)c;
                    } else if ((uint32_t)c > sndrow[gx]) {
                        sndrow[gx] = (uint32_t)c;
                    }
                }

                /* 离轴高光 */
                const int32_t gdx = dx - blob.glint_ox;
                const int32_t gdy = dy - blob.glint_oy;
                const uint32_t d2g = (uint32_t)(gdx * gdx + gdy * gdy);
                const int32_t tg = (int32_t)((d2g * (uint32_t)kg) >> 8);
                if (tg < FLUID_SMOOTH_LUT_SIZE) {
                    const int32_t g = sl[tg];
                    if (g > 0 && g > (int32_t)srow[gx]) {
                        srow[gx] = (uint8_t)(g >= 255 ? 255 : g);
                    }
                }
            }
        }
    }

    /* 后期处理：融合处变暗 + 软饱和（避免重叠区硬截断平台）+ 高光提亮 */
    const uint16_t *recip = self->recip;
    for (size_t i = 0; i < grid; i++) {
        const uint16_t s = sum[i];
        if (s == 0) {
            continue;
        }
        const uint32_t denom = maxc[i] + second[i];
        const uint32_t denom_i = denom > 65535u ? 65535u : denom;
        int32_t m = (int32_t)((maxc[i] * (uint32_t)recip[denom_i]) >> 8);
        if (m > 256) {
            m = 256;
        }
        const uint32_t dark = ((256u - m) * FLUID_CREASE_DARK_Q) >> 8;
        const int32_t s2 = (int32_t)((s * (256 - (int32_t)dark)) >> 8);
        const uint32_t den = (uint32_t)(s2 + FLUID_SOFT_SAT_K);
        const uint32_t q = (uint32_t)s2 * recip[den];
        int32_t v = (int32_t)(((q >> 8) * 255) >> 8);
        if (v > 255) {
            v = 255;
        }
        const uint8_t sv = spec[i];
        if (sv > 0) {
            v += sv >> FLUID_GLINT_ADD_SHIFT;
            if (v > 255) {
                v = 255;
            }
        }
        field[i] = (uint8_t)v;
    }
}

static inline uint8_t lerp_u8(uint8_t a, uint8_t b, uint32_t frac)
{
    return (uint8_t)((a * (256u - frac) + b * frac) >> 8);
}

/* 两步定点双线性放大：先水平 sim_w->h_res，再垂直 sim_h->v_res */
static void upscale(fluid_render_t *self, const uint8_t *field)
{
    const int sw = self->cfg.sim_w;
    const int sh = self->cfg.sim_h;
    const int hr = self->cfg.h_res;
    const int vr = self->cfg.v_res;
    uint8_t *temp = self->temp;

    const uint32_t hstep = ((uint32_t)(sw - 1) << 8) / (uint32_t)(hr - 1);
    for (int y = 0; y < sh; y++) {
        const uint8_t *src = field + y * sw;
        uint8_t *dst = temp + y * hr;
        uint32_t pos = 0;
        for (int x = 0; x < hr; x++) {
            const uint32_t i0 = pos >> 8;
            const uint32_t frac = pos & 0xFFu;
            const uint8_t a = src[i0];
            const uint8_t b = (i0 + 1 < (uint32_t)sw) ? src[i0 + 1] : a;
            dst[x] = lerp_u8(a, b, frac);
            pos += hstep;
        }
    }

    const uint32_t vstep = ((uint32_t)(sh - 1) << 8) / (uint32_t)(vr - 1);
    uint16_t *fb = self->frame_buf[self->current_fb];
    const uint16_t *palette = self->palette;
    uint32_t pos = 0;
    for (int y = 0; y < vr; y++) {
        const uint32_t i0 = pos >> 8;
        const uint32_t frac = pos & 0xFFu;
        const uint8_t *a_row = temp + i0 * hr;
        const uint8_t *b_row = (i0 + 1 < (uint32_t)sh) ? a_row + hr : a_row;
        uint16_t *out = fb + (size_t)y * hr;
        for (int x = 0; x < hr; x++) {
            const uint8_t v = lerp_u8(a_row[x], b_row[x], frac);
            out[x] = palette[v];
        }
        pos += vstep;
    }
}

static uint32_t irand(uint32_t range)
{
    return range > 0 ? esp_random() % range : 0;
}

static void init_blob(Blob *b, int sw, int sh, int base_r)
{
    b->x = (int32_t)irand(sw) << 8;
    b->y = (int32_t)irand(sh) << 8;
    int32_t vx = (int32_t)irand(FLUID_MAX_VEL * 2) - FLUID_MAX_VEL;
    int32_t vy = (int32_t)irand(FLUID_MAX_VEL * 2) - FLUID_MAX_VEL;
    if (vx > -16 && vx < 16) {
        vx = 16;
    }
    if (vy > -16 && vy < 16) {
        vy = 16;
    }
    b->vx = vx;
    b->vy = vy;
    const int32_t size_q = (int32_t)(FLUID_SIZE_VARY_MIN +
                                     irand(FLUID_SIZE_VARY_MAX - FLUID_SIZE_VARY_MIN + 1));
    const int32_t r2 = (base_r * base_r * size_q) / 100;
    b->r2_base = r2;
    b->r2_target = r2;
    b->r2_now = r2;
    b->ux = 256;
    b->uy = 0;
    b->stretch_q = 256;
    b->inv_stretch_q = 256;
    b->glint_ox = 0;
    b->glint_oy = 0;
    b->r2_glint = 0;
    b->phase = (uint8_t)irand(256);
    b->release_hold = 0;
    b->release_fade = 0;
    b->touched = false;
}

/* 液滴间凝聚力：远距离弱吸引聚拢，近距离排斥避免完全重叠。
 * 注意 d 是 8.8 定点（已含 <<8），range/near_d 必须同样左移 8 位再比较。 */
static void apply_cohesion(fluid_render_t *self)
{
    const int n = self->cfg.blob_count;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            Blob &a = self->blobs[i];
            Blob &b = self->blobs[j];
            const int32_t dx = b.x - a.x;
            const int32_t dy = b.y - a.y;
            const uint32_t d2 = (uint32_t)(dx * dx + dy * dy);
            if (d2 == 0) {
                continue;
            }
            const int32_t d = (int32_t)isqrt32(d2);
            const int32_t rsum = (int32_t)isqrt32((uint32_t)a.r2_now) +
                                 (int32_t)isqrt32((uint32_t)b.r2_now);
            const int32_t range = ((rsum * FLUID_COHESION_RANGE_Q) / 100) << 8;
            if (d > range) {
                continue;
            }
            const int32_t nx = (dx * 256) / d;
            const int32_t ny = (dy * 256) / d;
            const int32_t near_d = ((rsum * FLUID_COHESION_NEAR_Q) / 100) << 8;
            int32_t fx, fy;
            if (d < near_d) {
                const int32_t f = (FLUID_COHESION_REPEL_Q * (near_d - d)) / near_d;
                fx = -((nx * f) >> 8);
                fy = -((ny * f) >> 8);
            } else {
                const int32_t f = (FLUID_COHESION_ATTRACT_Q * (range - d)) / (range - near_d);
                fx = (nx * f) >> 8;
                fy = (ny * f) >> 8;
            }
            if (!a.touched) {
                a.vx += fx;
                a.vy += fy;
            }
            if (!b.touched) {
                b.vx -= fx;
                b.vy -= fy;
            }
        }
    }
}

static void step_sim(fluid_render_t *self)
{
    const int sw = self->cfg.sim_w;
    const int sh = self->cfg.sim_h;
    const int hr = self->cfg.h_res;
    const int vr = self->cfg.v_res;

    bool pressed = false;
    int tx = 0;
    int ty = 0;
    xSemaphoreTake(self->touch_mutex, portMAX_DELAY);
    pressed = self->touch_pressed;
    tx = self->touch_x;
    ty = self->touch_y;
    xSemaphoreGive(self->touch_mutex);

    /* 按下：绑定距手指最近的漂移液滴 */
    if (pressed && self->bound_blob < 0) {
        const int gx = (int)(((int64_t)tx * sw) / hr);
        const int gy = (int)(((int64_t)ty * sh) / vr);
        int best = -1;
        int64_t best_d2 = INT64_MAX;
        for (int i = 0; i < self->cfg.blob_count; i++) {
            const Blob &b = self->blobs[i];
            if (b.touched) {
                continue;
            }
            const int64_t dx = (b.x >> 8) - gx;
            const int64_t dy = (b.y >> 8) - gy;
            const int64_t d2 = dx * dx + dy * dy;
            if (d2 < best_d2) {
                best_d2 = d2;
                best = i;
            }
        }
        if (best >= 0) {
            self->blobs[best].touched = true;
            self->blobs[best].release_hold = 0;
            self->blobs[best].release_fade = 0;
            self->bound_blob = best;
        }
    }

    /* 抬起：解除绑定，短时保持高亮后衰减，并施加远离手指的推力 */
    if (!pressed && self->bound_blob >= 0) {
        const int idx = self->bound_blob;
        Blob &b = self->blobs[idx];
        b.touched = false;
        b.release_hold = FLUID_RELEASE_HOLD;
        b.release_fade = FLUID_RELEASE_FADE;
        const int gx = (int)(((int64_t)tx * sw) / hr);
        const int gy = (int)(((int64_t)ty * sh) / vr);
        b.vx = (int32_t)((b.x >> 8) - gx) * 24 + (int32_t)irand(64) - 32;
        b.vy = (int32_t)((b.y >> 8) - gy) * 24 + (int32_t)irand(64) - 32;
        if (b.vx > FLUID_MAX_VEL) {
            b.vx = FLUID_MAX_VEL;
        } else if (b.vx < -FLUID_MAX_VEL) {
            b.vx = -FLUID_MAX_VEL;
        }
        if (b.vy > FLUID_MAX_VEL) {
            b.vy = FLUID_MAX_VEL;
        } else if (b.vy < -FLUID_MAX_VEL) {
            b.vy = -FLUID_MAX_VEL;
        }
        self->bound_blob = -1;
    }

    int32_t disp_x[8] = {0};
    int32_t disp_y[8] = {0};

    for (int i = 0; i < self->cfg.blob_count; i++) {
        Blob &b = self->blobs[i];
        b.phase = (uint8_t)(b.phase + 4);

        if (b.touched && self->bound_blob == i) {
            const int gx = (int)(((int64_t)tx * sw) / hr);
            const int gy = (int)(((int64_t)ty * sh) / vr);
            const int32_t target_x = (int32_t)gx << 8;
            const int32_t target_y = (int32_t)gy << 8;
            disp_x[i] = ((target_x - b.x) * FLUID_TOUCH_LERP) >> 8;
            disp_y[i] = ((target_y - b.y) * FLUID_TOUCH_LERP) >> 8;
            b.x += disp_x[i];
            b.y += disp_y[i];
            b.r2_target = (b.r2_base * FLUID_TOUCH_RADIUS_X) / 100;
        } else {
            b.x += b.vx;
            b.y += b.vy;
            const int32_t minx = 1 << 8;
            const int32_t maxx = (int32_t)(sw - 1) << 8;
            const int32_t miny = 1 << 8;
            const int32_t maxy = (int32_t)(sh - 1) << 8;
            if (b.x < minx) {
                b.x = minx;
                b.vx = -b.vx;
            } else if (b.x > maxx) {
                b.x = maxx;
                b.vx = -b.vx;
            }
            if (b.y < miny) {
                b.y = miny;
                b.vy = -b.vy;
            } else if (b.y > maxy) {
                b.y = maxy;
                b.vy = -b.vy;
            }

            /* 墙排斥：液滴中心距墙小于自身半径时向内推，避免聚团贴角 */
            const int32_t rq = (int32_t)isqrt32((uint32_t)b.r2_now) << 8;
            if (b.x < rq) {
                b.vx += ((rq - b.x) * FLUID_WALL_K) >> 8;
            } else if (b.x > maxx - rq) {
                b.vx -= ((b.x - (maxx - rq)) * FLUID_WALL_K) >> 8;
            }
            if (b.y < rq) {
                b.vy += ((rq - b.y) * FLUID_WALL_K) >> 8;
            } else if (b.y > maxy - rq) {
                b.vy -= ((b.y - (maxy - rq)) * FLUID_WALL_K) >> 8;
            }

            if (b.release_hold > 0) {
                b.release_hold--;
                b.r2_target = (b.r2_base * FLUID_TOUCH_RADIUS_X) / 100;
            } else if (b.release_fade > 0) {
                b.release_fade--;
                const int32_t hi = (b.r2_base * FLUID_TOUCH_RADIUS_X) / 100;
                b.r2_target = b.r2_base + ((hi - b.r2_base) * b.release_fade) / FLUID_RELEASE_FADE;
            } else {
                b.r2_target = b.r2_base;
            }
        }
        b.r2_now += ((b.r2_target - b.r2_now) * FLUID_RADIUS_LERP) >> 8;
    }

    apply_cohesion(self);

    /* 阻尼 + 随机游走 + 最低速度 + 限速（触摸液滴除外） */
    const int32_t center_x = ((sw - 1) / 2) << 8;
    const int32_t center_y = ((sh - 1) / 2) << 8;
    for (int i = 0; i < self->cfg.blob_count; i++) {
        Blob &b = self->blobs[i];
        if (b.touched && self->bound_blob == i) {
            continue;
        }
        b.vx = (b.vx * FLUID_DAMPING_Q) >> 8;
        b.vy = (b.vy * FLUID_DAMPING_Q) >> 8;
        b.vx += (int32_t)irand(FLUID_JITTER_Q * 2 + 1) - FLUID_JITTER_Q;
        b.vy += (int32_t)irand(FLUID_JITTER_Q * 2 + 1) - FLUID_JITTER_Q;
        const uint32_t speed = isqrt32((uint32_t)(b.vx * b.vx + b.vy * b.vy));
        if (speed < FLUID_MIN_SPEED_Q) {
            b.vx += (int32_t)irand(24) - 12;
            b.vy += (int32_t)irand(24) - 12;
        }
        /* 向心力：把漂移的液滴/聚团缓慢拉回屏幕中央，避免静置后聚到墙角 */
        b.vx += ((center_x - b.x) * FLUID_CENTER_K) >> 16;
        b.vy += ((center_y - b.y) * FLUID_CENTER_K) >> 16;
        if (b.vx > FLUID_MAX_VEL) {
            b.vx = FLUID_MAX_VEL;
        } else if (b.vx < -FLUID_MAX_VEL) {
            b.vx = -FLUID_MAX_VEL;
        }
        if (b.vy > FLUID_MAX_VEL) {
            b.vy = FLUID_MAX_VEL;
        } else if (b.vy < -FLUID_MAX_VEL) {
            b.vy = -FLUID_MAX_VEL;
        }
    }

    for (int i = 0; i < self->cfg.blob_count; i++) {
        Blob &b = self->blobs[i];
        if (b.touched && self->bound_blob == i) {
            update_motion_params(b, disp_x[i], disp_y[i]);
        } else {
            update_motion_params(b, b.vx, b.vy);
        }
    }

    compute_field(self);
}

static void sim_task(void *arg)
{
    fluid_render_t *self = (fluid_render_t *)arg;
    const TickType_t period = pdMS_TO_TICKS(1000 / self->cfg.sim_hz);
    TickType_t last_wake = xTaskGetTickCount();
    while (self->running) {
        if (xSemaphoreTake(self->sem_field_consumed, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        const uint64_t t0 = esp_timer_get_time();
        step_sim(self);
        self->last_sim_us = esp_timer_get_time() - t0;
        xSemaphoreGive(self->sem_field_ready);
        xTaskDelayUntil(&last_wake, period);
    }
    /* 任务不自行删除，由 fluid_render_stop() 统一回收，避免句柄悬垂 */
    vTaskDelay(portMAX_DELAY);
}

static void render_task(void *arg)
{
    fluid_render_t *self = (fluid_render_t *)arg;
    const uint32_t field_bytes = (uint32_t)self->cfg.sim_w * self->cfg.sim_h;
    uint64_t frame_start = esp_timer_get_time();
    while (self->running) {
        if (xSemaphoreTake(self->sem_field_ready, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        memcpy(self->field_local, self->field, field_bytes);
        xSemaphoreGive(self->sem_field_consumed);

        uint64_t t0 = esp_timer_get_time();
        upscale(self, self->field_local);
        self->last_upscale_us = esp_timer_get_time() - t0;

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

        if (CONFIG_FLUID_RENDER_STATS_INTERVAL_MS > 0 && now >= self->stats_next_log_us) {
            const float avg = self->avg_frame_us > 1.0f ? self->avg_frame_us : 1.0f;
            ESP_LOGI(TAG,
                     "stats: fps=%.1f frame=%.2fms sim=%.2fms upscale=%.2fms submit=%.2fms",
                     1e6f / avg, avg / 1000.0f, (float)self->last_sim_us / 1000.0f,
                     (float)self->last_upscale_us / 1000.0f, (float)self->last_submit_us / 1000.0f);
            self->stats_next_log_us = now + (uint64_t)CONFIG_FLUID_RENDER_STATS_INTERVAL_MS * 1000;
        }
    }
    vTaskDelay(portMAX_DELAY);
}

#if CONFIG_FLUID_RENDER_DEBUG_COLOR_SEQ
static void fill_screen(fluid_render_t *self, uint16_t wire_color)
{
    uint16_t *fb = self->frame_buf[0];
    const int n = self->cfg.h_res * self->cfg.v_res;
    for (int i = 0; i < n; i++) {
        fb[i] = wire_color;
    }
    esp_lcd_panel_draw_bitmap(self->panel, 0, 0, self->cfg.h_res, self->cfg.v_res, fb);
}

static void debug_color_sequence(fluid_render_t *self)
{
    ESP_LOGI(TAG, "debug color sequence: red/green/blue (RGB565 byte-order check)");
    fill_screen(self, FLUID_RGB565_WIRE(255, 0, 0));
    vTaskDelay(pdMS_TO_TICKS(700));
    fill_screen(self, FLUID_RGB565_WIRE(0, 255, 0));
    vTaskDelay(pdMS_TO_TICKS(700));
    fill_screen(self, FLUID_RGB565_WIRE(0, 0, 255));
    vTaskDelay(pdMS_TO_TICKS(700));
    fill_screen(self, FLUID_RGB565_WIRE(0, 0, 0));
    vTaskDelay(pdMS_TO_TICKS(300));
}
#endif

fluid_render_config_t fluid_render_default_config(void)
{
    fluid_render_config_t cfg;
    cfg.h_res = 368;
    cfg.v_res = 448;
    cfg.sim_w = CONFIG_FLUID_RENDER_SIM_W;
    cfg.sim_h = CONFIG_FLUID_RENDER_SIM_H;
    cfg.sim_hz = CONFIG_FLUID_RENDER_SIM_HZ;
    cfg.blob_count = CONFIG_FLUID_RENDER_BLOB_COUNT;
    return cfg;
}

esp_err_t fluid_render_create(const fluid_render_config_t *config,
                              esp_lcd_panel_handle_t panel,
                              fluid_render_handle_t *out_handle)
{
    if (config == NULL || panel == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->h_res < 2 || config->v_res < 2 || config->sim_w < 2 || config->sim_h < 2 ||
        config->sim_w > config->h_res || config->sim_h > config->v_res ||
        config->blob_count < 1 || config->blob_count > 8 || config->sim_hz < 1 || config->sim_hz > 120) {
        return ESP_ERR_INVALID_ARG;
    }

    fluid_render_t *self = (fluid_render_t *)calloc(1, sizeof(fluid_render_t));
    if (self == NULL) {
        return ESP_ERR_NO_MEM;
    }
    self->cfg = *config;
    self->panel = panel;
    self->current_fb = 0;
    self->bound_blob = -1;
    self->avg_frame_us = 1e6f / (float)config->sim_hz;
    const int base_r = CONFIG_FLUID_RENDER_BLOB_RADIUS_CELLS;

    const size_t grid_bytes = (size_t)config->sim_w * config->sim_h;
    const size_t temp_bytes = (size_t)config->sim_h * config->h_res;
    const size_t fb_bytes = (size_t)config->h_res * config->v_res * sizeof(uint16_t);

    self->field = (uint8_t *)spiram_calloc(1, grid_bytes);
    self->field_local = (uint8_t *)spiram_calloc(1, grid_bytes);
    self->spec = (uint8_t *)spiram_calloc(1, grid_bytes);
    self->sum = (uint16_t *)spiram_calloc(grid_bytes, sizeof(uint16_t));
    self->maxc = (uint32_t *)spiram_calloc(grid_bytes, sizeof(uint32_t));
    self->second_maxc = (uint32_t *)spiram_calloc(grid_bytes, sizeof(uint32_t));
    self->temp = (uint8_t *)spiram_calloc(1, temp_bytes);
    self->frame_buf[0] = (uint16_t *)spiram_calloc(1, fb_bytes);
    self->frame_buf[1] = (uint16_t *)spiram_calloc(1, fb_bytes);
    self->frame_buf[2] = (uint16_t *)spiram_calloc(1, fb_bytes);
    self->palette = (uint16_t *)spiram_calloc(256, sizeof(uint16_t));
    self->recip = (uint16_t *)spiram_calloc(65536, sizeof(uint16_t));
    self->blobs = (Blob *)spiram_calloc((size_t)config->blob_count, sizeof(Blob));

    if (self->field == NULL || self->field_local == NULL || self->spec == NULL || self->sum == NULL ||
        self->maxc == NULL || self->second_maxc == NULL || self->temp == NULL ||
        self->frame_buf[0] == NULL || self->frame_buf[1] == NULL || self->frame_buf[2] == NULL ||
        self->palette == NULL ||
        self->recip == NULL || self->blobs == NULL) {
        goto fail;
    }

    self->sem_field_ready = xSemaphoreCreateBinary();
    self->sem_field_consumed = xSemaphoreCreateBinary();
    self->touch_mutex = xSemaphoreCreateMutex();
    if (self->sem_field_ready == NULL || self->sem_field_consumed == NULL || self->touch_mutex == NULL) {
        goto fail;
    }

    build_sin_lut();
    build_smooth_lut(self->smooth_lut);
    build_recip(self->recip);
    build_palette(self->palette);

    for (int i = 0; i < config->blob_count; i++) {
        init_blob(&self->blobs[i], config->sim_w, config->sim_h, base_r);
    }

    *out_handle = self;
    return ESP_OK;

fail:
    if (self->sem_field_ready) {
        vSemaphoreDelete(self->sem_field_ready);
    }
    if (self->sem_field_consumed) {
        vSemaphoreDelete(self->sem_field_consumed);
    }
    if (self->touch_mutex) {
        vSemaphoreDelete(self->touch_mutex);
    }
    heap_caps_free(self->field);
    heap_caps_free(self->field_local);
    heap_caps_free(self->spec);
    heap_caps_free(self->sum);
    heap_caps_free(self->maxc);
    heap_caps_free(self->second_maxc);
    heap_caps_free(self->temp);
    heap_caps_free(self->frame_buf[0]);
    heap_caps_free(self->frame_buf[1]);
    heap_caps_free(self->frame_buf[2]);
    heap_caps_free(self->palette);
    heap_caps_free(self->recip);
    heap_caps_free(self->blobs);
    free(self);
    return ESP_ERR_NO_MEM;
}

esp_err_t fluid_render_start(fluid_render_handle_t handle)
{
    if (handle == NULL || handle->running) {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_FLUID_RENDER_DEBUG_COLOR_SEQ
    debug_color_sequence(handle);
#endif

    handle->running = true;
    xSemaphoreGive(handle->sem_field_consumed);
    handle->stats_next_log_us = esp_timer_get_time() + (uint64_t)CONFIG_FLUID_RENDER_STATS_INTERVAL_MS * 1000;

    if (xTaskCreatePinnedToCore(sim_task, "fluid_sim", 4096, handle, 5, &handle->sim_task, 0) != pdPASS) {
        handle->running = false;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(render_task, "fluid_render", 4096, handle, 6, &handle->render_task, 1) != pdPASS) {
        handle->running = false;
        vTaskDelete(handle->sim_task);
        handle->sim_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t fluid_render_stop(fluid_render_handle_t handle)
{
    if (handle == NULL || !handle->running) {
        return ESP_ERR_INVALID_STATE;
    }
    handle->running = false;
    xSemaphoreGive(handle->sem_field_ready);
    xSemaphoreGive(handle->sem_field_consumed);
    vTaskDelay(pdMS_TO_TICKS(300));
    if (handle->sim_task) {
        vTaskDelete(handle->sim_task);
        handle->sim_task = NULL;
    }
    if (handle->render_task) {
        vTaskDelete(handle->render_task);
        handle->render_task = NULL;
    }
    return ESP_OK;
}

void fluid_render_touch_event(fluid_render_handle_t handle, bool pressed, uint16_t x, uint16_t y)
{
    if (handle == NULL) {
        return;
    }
    xSemaphoreTake(handle->touch_mutex, portMAX_DELAY);
    handle->touch_pressed = pressed;
    handle->touch_x = x;
    handle->touch_y = y;
    xSemaphoreGive(handle->touch_mutex);
}

esp_err_t fluid_render_get_stats(fluid_render_handle_t handle, fluid_render_stats_t *stats)
{
    if (handle == NULL || stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const float avg = handle->avg_frame_us > 1.0f ? handle->avg_frame_us : 1.0f;
    stats->fps = 1e6f / avg;
    stats->avg_frame_us = handle->avg_frame_us;
    stats->sim_us = (float)handle->last_sim_us;
    stats->upscale_us = (float)handle->last_upscale_us;
    stats->submit_us = (float)handle->last_submit_us;
    return ESP_OK;
}
