#pragma once

#include <string.h>
#include <time.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// ESP32-P4-WIFI6-Touch-LCD-3.4C uses an 800x800 round panel. The themes keep
// their original 466x466 design grid but convert every coordinate at compile
// time, so text and vector shapes are rendered natively at 800x800 instead of
// being upscaled by a parent transform.
static constexpr int32_t CLOCK_THEME_SIZE = 800;
static constexpr int32_t CLOCK_THEME_DESIGN_SIZE = 466;
static constexpr int32_t CLOCK_THEME_SCALE_256 = 439;

static constexpr int32_t clock_theme_px(int32_t design_px)
{
    return design_px >= 0
               ? (int32_t)(design_px * (float)CLOCK_THEME_SIZE / (float)CLOCK_THEME_DESIGN_SIZE + 0.5f)
               : (int32_t)(design_px * (float)CLOCK_THEME_SIZE / (float)CLOCK_THEME_DESIGN_SIZE - 0.5f);
}

static constexpr float clock_theme_pxf(float design_px)
{
    return design_px * (float)CLOCK_THEME_SIZE / (float)CLOCK_THEME_DESIGN_SIZE;
}

// 可插拔时间页面主题。build 在 parent 下构建 UI 子树并返回根容器，
// update 返回根内容是否变化，引擎仅在需要时刷新截图。
typedef struct {
    const char *name;
    lv_obj_t *(*build)(lv_obj_t *parent);
    bool (*update)(const struct tm *timeinfo);
    void (*reset)(void);
    void (*destroy_overlay)(void);
    bool (*tick)(uint32_t now_ms);
    bool (*tick_updates_overlay_only)(void);
    void (*set_overlay_visible)(bool visible);
    void (*set_rotation)(float angle_deg);
} clock_theme_t;

// Common helper: avoid freeing and reallocating a label string when its text
// did not change. Themes update once per second, while dates/weekdays change
// much less often.
static inline bool clock_theme_set_label_text(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return false;
    }
    const char *current = lv_label_get_text(label);
    if (current != NULL && strcmp(current, text) == 0) {
        return false;
    }
    lv_label_set_text(label, text);
    return true;
}

extern const clock_theme_t g_clock_theme_minimal;
extern const clock_theme_t g_clock_theme_segment;
extern const clock_theme_t g_clock_theme_analog;
extern const clock_theme_t g_clock_theme_m5_classic;
extern const clock_theme_t g_clock_theme_m5_simple;
extern const clock_theme_t g_clock_theme_m5_big_number;
extern const clock_theme_t g_clock_theme_m5_number_flow;
extern const clock_theme_t g_clock_theme_dot_matrix;

#ifdef __cplusplus
}
#endif
