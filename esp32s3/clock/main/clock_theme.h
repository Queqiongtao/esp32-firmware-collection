#pragma once

#include <string.h>
#include <time.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

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
