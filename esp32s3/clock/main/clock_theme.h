#pragma once

#include <time.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// 可插拔时间页面主题。build 在 parent 下构建 UI 子树并返回根容器，
// 引擎据此截图旋转；切换主题时引擎删除旧根、调用下一个主题的 build。
typedef struct {
    const char *name;
    lv_obj_t *(*build)(lv_obj_t *parent);
    void (*update)(const struct tm *timeinfo);
    void (*reset)(void);
    bool (*tick)(uint32_t now_ms);
} clock_theme_t;

extern const clock_theme_t g_clock_theme_minimal;
extern const clock_theme_t g_clock_theme_segment;
extern const clock_theme_t g_clock_theme_analog;
extern const clock_theme_t g_clock_theme_outline_analog;
extern const clock_theme_t g_clock_theme_mooncat;
extern const clock_theme_t g_clock_theme_m5_classic;
extern const clock_theme_t g_clock_theme_m5_simple;
extern const clock_theme_t g_clock_theme_m5_big_number;
extern const clock_theme_t g_clock_theme_m5_number_flow;

#ifdef __cplusplus
}
#endif
