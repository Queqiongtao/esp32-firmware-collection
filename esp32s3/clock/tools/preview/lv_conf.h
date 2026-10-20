/**
 * @file lv_conf.h
 * 主机端主题预览专用配置。
 *
 * 只服务于 tools/preview，不参与 ESP-IDF 构建。
 * 关键项与设备端 sdkconfig 保持一致：RGB565、Montserrat 12~40。
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/* 与设备端一致：RGB565 */
#define LV_COLOR_DEPTH 16

/* 单线程、标准库内存，避免固定堆不够渲染 466 x 466 */
#define LV_USE_OS LV_OS_NONE
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

/* 日志 */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/* 主题用到 / 预览需要的模块 */
#define LV_USE_SNAPSHOT 1
#define LV_USE_ARC 1
#define LV_USE_CANVAS 1
#define LV_USE_IMAGE 1
#define LV_USE_LABEL 1
#define LV_USE_LINE 1
#define LV_USE_SCALE 1

/* 设备端启用的 Montserrat 字号 */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* 软件渲染 */
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_SUPPORT_RGB565 1

#endif /* LV_CONF_H */
