/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "lvgl.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl_demo_font_patch.h"
#include "test_audio.h"
#include "test_rtc.h"
#include "test_usb.h"

static const char *TAG = "demo_ui";

typedef struct {
    uint32_t touch_count;
    uint32_t last_touch_ms;
    lv_point_t last_point;
    bool touch_active;
} demo_touch_state_t;

static lv_obj_t *s_touch_status_label;
static lv_obj_t *s_touch_count_label;
static lv_obj_t *s_touch_point_label;
static lv_obj_t *s_touch_hint_label;

static lv_obj_t *s_rtc_status_label;
static lv_obj_t *s_rtc_time_label;
static lv_obj_t *s_rtc_sync_label;

static lv_obj_t *s_usb_status_label;
static lv_obj_t *s_usb_line_label;
static lv_obj_t *s_usb_stats_label;
static lv_obj_t *s_usb_hint_label;

static lv_obj_t *s_audio_status_label;
static lv_obj_t *s_audio_size_label;
static lv_obj_t *s_audio_peak_label;
static lv_obj_t *s_audio_input_label;
static lv_obj_t *s_audio_music_btn;
static lv_obj_t *s_audio_record_btn;
static lv_obj_t *s_audio_play_btn;

static demo_touch_state_t s_touch_state;

static const lv_font_t *demo_get_ui_font(void)
{
    return &lvgl_demo_font_patch_16;
}

static lv_obj_t *demo_create_card(lv_obj_t *parent, const char *title, lv_coord_t width, lv_coord_t height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, width, height);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x1E3A5F), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xF8F2E7), 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, demo_get_ui_font(), 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x132238), 0);
    return card;
}

static const char *audio_state_to_text(test_audio_state_t state)
{
    switch (state) {
    case TEST_AUDIO_STATE_UNINITIALIZED:
        return "未初始化";
    case TEST_AUDIO_STATE_IDLE:
        return "空闲";
    case TEST_AUDIO_STATE_RECORDING:
        return "录音中";
    case TEST_AUDIO_STATE_RECORDED:
        return "已录音";
    case TEST_AUDIO_STATE_PLAYING:
        return "播放录音中";
    case TEST_AUDIO_STATE_MUSIC_PLAYING:
        return "播放内置音乐中";
    case TEST_AUDIO_STATE_ERROR:
        return "错误";
    default:
        return "未知";
    }
}

static const char *audio_input_level_text(uint16_t peak, size_t nonzero_samples)
{
    if (peak == 0 || nonzero_samples == 0) {
        return "静音";
    }
    if (peak < 256) {
        return "弱";
    }
    return "活跃";
}

static void demo_touch_area_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *target = lv_event_get_target_obj(e);
    lv_indev_t *indev = lv_event_get_indev(e);

    if (indev == NULL) {
        return;
    }

    lv_indev_get_point(indev, &s_touch_state.last_point);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        s_touch_state.touch_active = true;
        lv_obj_set_style_bg_color(target, lv_color_hex(0xF4B942), 0);
    } else if (code == LV_EVENT_RELEASED) {
        s_touch_state.touch_active = false;
        lv_obj_set_style_bg_color(target, lv_color_hex(0x8EC5A4), 0);
    } else if (code == LV_EVENT_CLICKED) {
        s_touch_state.touch_count++;
        s_touch_state.last_touch_ms = lv_tick_get();
        lv_obj_set_style_bg_color(target, lv_color_hex(0xD97A3A), 0);
    }
}

static void audio_music_btn_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = test_audio_start_music();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start music failed: %s", esp_err_to_name(ret));
    }
}

static void audio_record_btn_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = test_audio_start_record();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start record failed: %s", esp_err_to_name(ret));
    }
}

static void audio_play_btn_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = test_audio_start_playback();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start playback failed: %s", esp_err_to_name(ret));
    }
}

static void demo_update_touch_card(void)
{
    if (s_touch_status_label == NULL) {
        return;
    }

    lv_label_set_text_fmt(s_touch_status_label, "状态: %s", s_touch_state.touch_active ? "按下中" : "待触摸");
    lv_label_set_text_fmt(s_touch_count_label, "点击次数: %" PRIu32, s_touch_state.touch_count);
    lv_label_set_text_fmt(s_touch_point_label, "最近坐标: (%" PRId32 ", %" PRId32 ")",
                          s_touch_state.last_point.x, s_touch_state.last_point.y);

    if (s_touch_state.last_touch_ms == 0) {
        lv_label_set_text(s_touch_hint_label, "在下方触摸区点击，验证显示与触摸联动");
    } else {
        lv_label_set_text_fmt(s_touch_hint_label, "最近点击: %" PRIu32 " ms", s_touch_state.last_touch_ms);
    }
}

static void demo_update_rtc_card(void)
{
    if (s_rtc_status_label == NULL) {
        return;
    }

    test_rtc_status_t status = {0};
    esp_err_t ret = test_rtc_get_status(&status);
    if (ret != ESP_OK || !status.initialized) {
        lv_label_set_text(s_rtc_status_label, "状态: 未初始化");
        lv_label_set_text(s_rtc_time_label, "时间: --");
        lv_label_set_text(s_rtc_sync_label, "同步: --");
        return;
    }

    if (status.time_valid) {
        lv_label_set_text(s_rtc_status_label, "状态: 运行中");
        lv_label_set_text_fmt(s_rtc_time_label, "时间: %04u-%02u-%02u %02u:%02u:%02u",
                              status.year, status.month, status.mday,
                              status.hour, status.min, status.sec);
    } else {
        lv_label_set_text_fmt(s_rtc_status_label, "状态: 错误(0x%x)", (unsigned int)status.last_error);
        lv_label_set_text(s_rtc_time_label, "时间: 无有效值");
    }

    lv_label_set_text_fmt(s_rtc_sync_label, "同步: %s",
                          status.synced_from_build_time ? "曾从固件构建时间写入" : "保持 RTC 原值");
}

static void demo_update_usb_card(void)
{
    if (s_usb_status_label == NULL) {
        return;
    }

    test_usb_status_t status = {0};
    esp_err_t ret = test_usb_get_status(&status);
    if (ret != ESP_OK || !status.initialized) {
        lv_label_set_text(s_usb_status_label, "状态: 未初始化");
        lv_label_set_text(s_usb_line_label, "线路: --");
        lv_label_set_text(s_usb_stats_label, "收发: --");
        return;
    }

    if (status.last_error != ESP_OK) {
        lv_label_set_text_fmt(s_usb_status_label, "状态: 错误(0x%x)", (unsigned int)status.last_error);
    } else {
        lv_label_set_text_fmt(s_usb_status_label, "状态: %s", status.attached ? "已连接主机" : "等待枚举");
    }

    lv_label_set_text_fmt(s_usb_line_label, "DTR=%d RTS=%d 最近收包=%uB 最近发包=%uB",
                          status.dtr, status.rts,
                          (unsigned int)status.last_rx_size, (unsigned int)status.last_tx_size);
    lv_label_set_text_fmt(s_usb_stats_label, "RX %" PRIu32 " 包/%" PRIu32 " B  TX %" PRIu32 " 包/%" PRIu32 " B",
                          status.rx_packets, status.rx_bytes, status.tx_packets, status.tx_bytes);
    lv_label_set_text(s_usb_hint_label, "连接 HS USB Device 口，用串口工具发送数据验证 CDC 回显");
}

static void demo_update_audio_card(void)
{
    if (s_audio_status_label == NULL) {
        return;
    }

    test_audio_status_t status = {0};
    esp_err_t ret = test_audio_get_status(&status);
    if (ret != ESP_OK || !status.initialized) {
        lv_label_set_text(s_audio_status_label, "状态: 未初始化");
        lv_label_set_text(s_audio_size_label, "录音数据: --");
        lv_label_set_text(s_audio_peak_label, "峰值: --");
        lv_label_set_text(s_audio_input_label, "输入电平: --");
        return;
    }

    if (status.state == TEST_AUDIO_STATE_ERROR) {
        lv_label_set_text_fmt(s_audio_status_label, "状态: 错误(0x%x)", (unsigned int)status.last_error);
    } else {
        lv_label_set_text_fmt(s_audio_status_label, "状态: %s", audio_state_to_text(status.state));
    }

    lv_label_set_text_fmt(s_audio_size_label, "录音数据: %u B", (unsigned int)status.last_record_size);
    lv_label_set_text_fmt(s_audio_peak_label, "峰值: %u", (unsigned int)status.last_record_peak);
    lv_label_set_text_fmt(s_audio_input_label, "输入电平: %s", audio_input_level_text(status.last_record_peak,
                                                                                      status.last_record_nonzero_count));

    bool can_start_new = (status.state == TEST_AUDIO_STATE_IDLE ||
                          status.state == TEST_AUDIO_STATE_RECORDED ||
                          status.state == TEST_AUDIO_STATE_ERROR);
    bool can_play_record = (status.state == TEST_AUDIO_STATE_RECORDED && status.last_record_size > 0);

    if (can_start_new) {
        lv_obj_clear_state(s_audio_music_btn, LV_STATE_DISABLED);
        lv_obj_clear_state(s_audio_record_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_audio_music_btn, LV_STATE_DISABLED);
        lv_obj_add_state(s_audio_record_btn, LV_STATE_DISABLED);
    }

    if (can_play_record) {
        lv_obj_clear_state(s_audio_play_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_audio_play_btn, LV_STATE_DISABLED);
    }
}

static void demo_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    demo_update_touch_card();
    demo_update_rtc_card();
    demo_update_usb_card();
    demo_update_audio_card();
}

void example_lvgl_demo_ui(lv_display_t *disp)
{
    lv_theme_default_init(disp,
                          lv_color_hex(0x1F5C8A),
                          lv_color_hex(0xD97A3A),
                          false,
                          demo_get_ui_font());

    lv_obj_t *screen = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xE6D8C2), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0xF6EFE4), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_pad_all(screen, 18, 0);
    lv_obj_set_style_pad_row(screen, 16, 0);
    lv_obj_set_layout(screen, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(screen, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_text_font(screen, demo_get_ui_font(), 0);

    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(header, 20, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x17324D), 0);
    lv_obj_set_style_text_color(header, lv_color_hex(0xFFF8ED), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 16, 0);
    lv_obj_set_style_pad_row(header, 8, 0);
    lv_obj_set_layout(header, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "JC1060P470 综合验证 Demo");
    lv_obj_set_style_text_font(title, demo_get_ui_font(), 0);

    lv_obj_t *subtitle = lv_label_create(header);
    lv_label_set_text(subtitle, "显示/触摸 + RTC + USB Device CDC + 音频播放录音回放");

    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_set_style_pad_row(row, 16, 0);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *touch_card = demo_create_card(row, "1. 显示与触摸", 488, 304);
    s_touch_status_label = lv_label_create(touch_card);
    s_touch_count_label = lv_label_create(touch_card);
    s_touch_point_label = lv_label_create(touch_card);
    s_touch_hint_label = lv_label_create(touch_card);

    lv_obj_t *touch_pad = lv_obj_create(touch_card);
    lv_obj_set_size(touch_pad, lv_pct(100), 110);
    lv_obj_set_style_radius(touch_pad, 14, 0);
    lv_obj_set_style_bg_color(touch_pad, lv_color_hex(0x8EC5A4), 0);
    lv_obj_set_style_border_width(touch_pad, 2, 0);
    lv_obj_set_style_border_color(touch_pad, lv_color_hex(0x3E6B55), 0);
    lv_obj_clear_flag(touch_pad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(touch_pad, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(touch_pad, demo_touch_area_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(touch_pad, demo_touch_area_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(touch_pad, demo_touch_area_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(touch_pad, demo_touch_area_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *touch_pad_label = lv_label_create(touch_pad);
    lv_label_set_text(touch_pad_label, "触摸这里");
    lv_obj_center(touch_pad_label);

    lv_obj_t *rtc_card = demo_create_card(row, "2. RTC", 488, 230);
    s_rtc_status_label = lv_label_create(rtc_card);
    s_rtc_time_label = lv_label_create(rtc_card);
    s_rtc_sync_label = lv_label_create(rtc_card);

    lv_obj_t *usb_card = demo_create_card(row, "3. USB Device", 488, 230);
    s_usb_status_label = lv_label_create(usb_card);
    s_usb_line_label = lv_label_create(usb_card);
    s_usb_stats_label = lv_label_create(usb_card);
    s_usb_hint_label = lv_label_create(usb_card);
    lv_obj_set_style_text_color(s_usb_hint_label, lv_color_hex(0x5E4C38), 0);

    lv_obj_t *audio_card = demo_create_card(row, "4. 音频", 488, 230);
    s_audio_status_label = lv_label_create(audio_card);
    s_audio_size_label = lv_label_create(audio_card);
    s_audio_peak_label = lv_label_create(audio_card);
    s_audio_input_label = lv_label_create(audio_card);

    lv_obj_t *audio_btn_row = lv_obj_create(audio_card);
    lv_obj_set_width(audio_btn_row, lv_pct(100));
    lv_obj_set_height(audio_btn_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(audio_btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(audio_btn_row, 0, 0);
    lv_obj_set_style_pad_all(audio_btn_row, 0, 0);
    lv_obj_set_style_pad_column(audio_btn_row, 10, 0);
    lv_obj_set_layout(audio_btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(audio_btn_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_clear_flag(audio_btn_row, LV_OBJ_FLAG_SCROLLABLE);

    s_audio_music_btn = lv_button_create(audio_btn_row);
    lv_obj_set_size(s_audio_music_btn, 135, 46);
    lv_obj_add_event_cb(s_audio_music_btn, audio_music_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *music_label = lv_label_create(s_audio_music_btn);
    lv_label_set_text(music_label, "播放内置音乐");
    lv_obj_center(music_label);

    s_audio_record_btn = lv_button_create(audio_btn_row);
    lv_obj_set_size(s_audio_record_btn, 110, 46);
    lv_obj_add_event_cb(s_audio_record_btn, audio_record_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *record_label = lv_label_create(s_audio_record_btn);
    lv_label_set_text(record_label, "开始录音");
    lv_obj_center(record_label);

    s_audio_play_btn = lv_button_create(audio_btn_row);
    lv_obj_set_size(s_audio_play_btn, 110, 46);
    lv_obj_add_event_cb(s_audio_play_btn, audio_play_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *play_label = lv_label_create(s_audio_play_btn);
    lv_label_set_text(play_label, "播放录音");
    lv_obj_center(play_label);

    demo_update_touch_card();
    demo_update_rtc_card();
    demo_update_usb_card();
    demo_update_audio_card();
    lv_timer_create(demo_ui_timer_cb, 250, NULL);
}
