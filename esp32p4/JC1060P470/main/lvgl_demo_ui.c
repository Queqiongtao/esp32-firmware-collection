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
#include "test_battery.h"
#include "test_rtc.h"
#include "test_usb.h"
#include "test_wireless.h"

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

static lv_obj_t *s_battery_status_label;
static lv_obj_t *s_battery_adc_label;
static lv_obj_t *s_battery_voltage_label;
static lv_obj_t *s_battery_percent_label;
static lv_obj_t *s_battery_hint_label;

static lv_obj_t *s_wifi_status_label;
static lv_obj_t *s_wifi_link_label;
static lv_obj_t *s_wifi_ap_label;
static lv_obj_t *s_wifi_hint_label;
static lv_obj_t *s_wifi_scan_btn;
static lv_obj_t *s_wifi_list;
static lv_obj_t *s_ble_status_label;
static lv_obj_t *s_ble_hint_label;
static lv_obj_t *s_wifi_dialog_mask;
static lv_obj_t *s_wifi_dialog;
static lv_obj_t *s_wifi_dialog_ssid_label;
static lv_obj_t *s_wifi_password_ta;
static lv_obj_t *s_wifi_keyboard;

static demo_touch_state_t s_touch_state;
static test_wireless_ap_record_t s_wifi_ap_records[TEST_WIRELESS_SCAN_RESULT_MAX];
static size_t s_wifi_ap_record_count;
static uint32_t s_wifi_scan_generation;
static char s_selected_wifi_ssid[TEST_WIRELESS_SSID_MAX_LEN + 1];

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

static const char *wireless_transport_state_to_text(test_wireless_transport_state_t state)
{
    switch (state) {
    case TEST_WIRELESS_TRANSPORT_UNINITIALIZED:
        return "未初始化";
    case TEST_WIRELESS_TRANSPORT_NOT_AVAILABLE:
        return "host NA";
    case TEST_WIRELESS_TRANSPORT_IDLE:
        return "空闲";
    case TEST_WIRELESS_TRANSPORT_CONNECTING:
        return "连接中";
    case TEST_WIRELESS_TRANSPORT_READY:
        return "已就绪";
    case TEST_WIRELESS_TRANSPORT_ERROR:
        return "错误";
    default:
        return "未知";
    }
}

static const char *wireless_wifi_state_to_text(test_wireless_wifi_state_t state)
{
    switch (state) {
    case TEST_WIRELESS_WIFI_UNINITIALIZED:
        return "未初始化";
    case TEST_WIRELESS_WIFI_NOT_AVAILABLE:
        return "host NA";
    case TEST_WIRELESS_WIFI_IDLE:
        return "空闲";
    case TEST_WIRELESS_WIFI_SCANNING:
        return "Scan";
    case TEST_WIRELESS_WIFI_CONNECTING:
        return "Join";
    case TEST_WIRELESS_WIFI_CONNECTED:
        return "已连接";
    case TEST_WIRELESS_WIFI_ERROR:
        return "错误";
    default:
        return "未知";
    }
}

static const char *wireless_ble_state_to_text(test_wireless_ble_state_t state)
{
    switch (state) {
    case TEST_WIRELESS_BLE_UNINITIALIZED:
        return "未初始化";
    case TEST_WIRELESS_BLE_NOT_AVAILABLE:
        return "host NA";
    case TEST_WIRELESS_BLE_IDLE:
        return "空闲";
    case TEST_WIRELESS_BLE_SCANNING:
        return "Scan";
    case TEST_WIRELESS_BLE_ADVERTISING:
        return "Adv On";
    case TEST_WIRELESS_BLE_ERROR:
        return "错误";
    default:
        return "未知";
    }
}

static const char *wireless_wifi_security_text(bool secured)
{
    return secured ? "Lock" : "Open";
}

static void wifi_dialog_close(void);
static void wifi_dialog_open(const char *ssid);
static void demo_refresh_wifi_ap_list(const test_wireless_status_t *status);

static void wireless_wifi_scan_btn_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = test_wireless_request_wifi_scan();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan request failed: %s", esp_err_to_name(ret));
    }
}

static void wifi_password_ta_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        if (s_wifi_keyboard != NULL && s_wifi_password_ta != NULL) {
            lv_keyboard_set_textarea(s_wifi_keyboard, s_wifi_password_ta);
        }
    }
}

static void wifi_dialog_connect_btn_cb(lv_event_t *e)
{
    (void)e;
#if CONFIG_EXAMPLE_ENABLE_TEST_WIRELESS && CONFIG_EXAMPLE_WIRELESS_ENABLE_WIFI
    const char *password = s_wifi_password_ta != NULL ? lv_textarea_get_text(s_wifi_password_ta) : "";
    esp_err_t ret = test_wireless_request_wifi_connect(s_selected_wifi_ssid, password);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi connect request failed: %s", esp_err_to_name(ret));
        return;
    }
    wifi_dialog_close();
#endif
}

static void wifi_dialog_cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    wifi_dialog_close();
}

static void wifi_keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        wifi_dialog_connect_btn_cb(e);
    } else if (code == LV_EVENT_CANCEL) {
        wifi_dialog_close();
    }
}

static void wifi_ap_item_event_cb(lv_event_t *e)
{
    const char *ssid = lv_event_get_user_data(e);
    if (ssid != NULL && ssid[0] != '\0') {
        wifi_dialog_open(ssid);
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

static void demo_update_battery_card(void)
{
    if (s_battery_status_label == NULL) {
        return;
    }

#if CONFIG_EXAMPLE_ENABLE_TEST_BATTERY
    const test_battery_status_t *status = test_battery_get_status();
    if (status == NULL || !status->initialized) {
        lv_label_set_text(s_battery_status_label, "状态: 初始化中");
        lv_label_set_text(s_battery_adc_label, "ADC值: --");
        lv_label_set_text(s_battery_voltage_label, "BAT值: --");
        lv_label_set_text(s_battery_percent_label, "BAT%: --");
        lv_label_set_text(s_battery_hint_label, "GPIO52 BAT 点");
        return;
    }

    esp_err_t ret = test_battery_run();

    if (!status->sample_valid) {
        lv_label_set_text_fmt(s_battery_status_label, "状态: %s(0x%x)",
                              ret == ESP_OK ? "采样中" : "错误",
                              (unsigned int)status->last_error);
        lv_label_set_text(s_battery_adc_label, "ADC值: --");
        lv_label_set_text(s_battery_voltage_label, "BAT值: --");
        lv_label_set_text(s_battery_percent_label, "BAT%: --");
    } else {
        lv_label_set_text_fmt(s_battery_status_label, "状态: %s", status->calibration_enabled ? "运行中(CAL)" : "运行中(RAW)");
        lv_label_set_text_fmt(s_battery_adc_label, "ADC值: %d.%03d V",
                              status->adc_voltage_mv / 1000, status->adc_voltage_mv % 1000);
        lv_label_set_text_fmt(s_battery_voltage_label, "BAT值: %d.%03d V",
                              status->battery_voltage_mv / 1000, status->battery_voltage_mv % 1000);
        lv_label_set_text_fmt(s_battery_percent_label, "BAT%%: %d%%", status->battery_percent);
    }

    lv_label_set_text_fmt(s_battery_hint_label, "次数: %" PRIu32, status->sample_count);
#else
    lv_label_set_text(s_battery_status_label, "状态: 未开");
    lv_label_set_text(s_battery_adc_label, "ADC值: --");
    lv_label_set_text(s_battery_voltage_label, "BAT值: --");
    lv_label_set_text(s_battery_percent_label, "BAT%: --");
    lv_label_set_text(s_battery_hint_label, "开 CONFIG_EXAMPLE_ENABLE_TEST_BATTERY");
#endif
}

static void demo_refresh_wifi_ap_list(const test_wireless_status_t *status)
{
    if (s_wifi_list == NULL || status == NULL) {
        return;
    }

    if (!status->transport_confirmed) {
        lv_obj_clean(s_wifi_list);
        lv_list_add_text(s_wifi_list, "C6 Link Not Ready");
        s_wifi_ap_record_count = 0;
        s_wifi_scan_generation = status->wifi_scan_generation;
        return;
    }

    if (!status->wifi_scan_results_valid && status->wifi_state != TEST_WIRELESS_WIFI_SCANNING) {
        lv_obj_clean(s_wifi_list);
        lv_list_add_text(s_wifi_list, "Tap Scan");
        s_wifi_ap_record_count = 0;
        s_wifi_scan_generation = status->wifi_scan_generation;
        return;
    }

    if (status->wifi_state == TEST_WIRELESS_WIFI_SCANNING) {
        lv_obj_clean(s_wifi_list);
        lv_list_add_text(s_wifi_list, "Scanning...");
        return;
    }

    if (status->wifi_scan_generation == s_wifi_scan_generation) {
        return;
    }

    size_t ap_count = 0;
    esp_err_t ret = test_wireless_get_scan_results(s_wifi_ap_records,
                                                   TEST_WIRELESS_SCAN_RESULT_MAX,
                                                   &ap_count);
    if (ret != ESP_OK) {
        lv_obj_clean(s_wifi_list);
        lv_list_add_text(s_wifi_list, "Scan Read Fail");
        return;
    }

    lv_obj_clean(s_wifi_list);
    s_wifi_ap_record_count = ap_count;
    s_wifi_scan_generation = status->wifi_scan_generation;

    if (ap_count == 0) {
        lv_list_add_text(s_wifi_list, "Scan Done, 0 AP");
        return;
    }

    for (size_t i = 0; i < ap_count; i++) {
        char item_text[96];
        const char *ssid = s_wifi_ap_records[i].ssid[0] != '\0' ? s_wifi_ap_records[i].ssid : "<Hidden>";
        snprintf(item_text, sizeof(item_text), "%s  %ddBm  %s",
                 ssid, s_wifi_ap_records[i].rssi, wireless_wifi_security_text(s_wifi_ap_records[i].secured));
        lv_obj_t *item = lv_list_add_button(s_wifi_list, NULL, item_text);
        if (s_wifi_ap_records[i].ssid[0] != '\0') {
            lv_obj_add_event_cb(item, wifi_ap_item_event_cb, LV_EVENT_CLICKED, s_wifi_ap_records[i].ssid);
        } else {
            lv_obj_add_state(item, LV_STATE_DISABLED);
        }
    }
}

static void wifi_dialog_close(void)
{
    if (s_wifi_keyboard != NULL) {
        lv_keyboard_set_textarea(s_wifi_keyboard, NULL);
        lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_dialog_mask != NULL) {
        lv_obj_add_flag(s_wifi_dialog_mask, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_password_ta != NULL) {
        lv_textarea_set_text(s_wifi_password_ta, "");
    }
    s_selected_wifi_ssid[0] = '\0';
}

static void wifi_dialog_open(const char *ssid)
{
    if (s_wifi_dialog_mask == NULL || s_wifi_dialog_ssid_label == NULL ||
        s_wifi_password_ta == NULL || s_wifi_keyboard == NULL || ssid == NULL || ssid[0] == '\0') {
        return;
    }

    snprintf(s_selected_wifi_ssid, sizeof(s_selected_wifi_ssid), "%s", ssid);
    lv_label_set_text_fmt(s_wifi_dialog_ssid_label, "SSID: %s", ssid);
    lv_textarea_set_text(s_wifi_password_ta, "");
    lv_obj_clear_flag(s_wifi_dialog_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_wifi_keyboard, s_wifi_password_ta);
    lv_obj_move_foreground(s_wifi_dialog_mask);
    lv_obj_move_foreground(s_wifi_dialog);
    lv_obj_move_foreground(s_wifi_keyboard);
    lv_obj_add_state(s_wifi_password_ta, LV_STATE_FOCUSED);
}

static void demo_update_wireless_card(void)
{
    if (s_wifi_status_label == NULL || s_ble_status_label == NULL) {
        return;
    }

#if CONFIG_EXAMPLE_ENABLE_TEST_WIRELESS
    test_wireless_status_t status = {0};
    esp_err_t ret = test_wireless_get_status(&status);
    if (ret != ESP_OK) {
        lv_label_set_text(s_wifi_status_label, "状态: 未初始化");
        lv_label_set_text(s_wifi_link_label, "Link: --");
        lv_label_set_text(s_wifi_ap_label, "WiFi: --");
        lv_label_set_text(s_wifi_hint_label, "Hint: wireless NA");
        lv_label_set_text(s_ble_status_label, "状态: 未初始化");
        lv_label_set_text(s_ble_hint_label, "BLE wireless NA");
        lv_obj_add_state(s_wifi_scan_btn, LV_STATE_DISABLED);
        if (s_wifi_list != NULL) {
            lv_obj_clean(s_wifi_list);
            lv_list_add_text(s_wifi_list, "WiFi Not Ready");
        }
        return;
    }

    if (!status.initialized) {
        if (status.init_attempted) {
            lv_label_set_text_fmt(s_wifi_status_label, "状态: %s(0x%x)",
                                  wireless_transport_state_to_text(status.transport_state),
                                  (unsigned int)status.last_error);
            lv_label_set_text_fmt(s_wifi_link_label, "Link: %s FW:%s RPC:%s",
                                  status.cp_target_name[0] != '\0' ? status.cp_target_name : "--",
                                  status.cp_fw_version[0] != '\0' ? status.cp_fw_version : "--",
                                  status.rpc_metadata_ready ? "OK" : "WARN");
            lv_label_set_text_fmt(s_wifi_ap_label, "WiFi: %s up=%d init=%d",
                                  wireless_wifi_state_to_text(status.wifi_state),
                                  status.transport_up_received,
                                  status.cp_init_received);
            lv_label_set_text_fmt(s_wifi_hint_label, "Hint: %s evt=%" PRIi32 " cp=0x%x fw=0x%x",
                                  status.last_hint,
                                  status.hosted_last_event,
                                  (unsigned int)status.hosted_cp_info_error,
                                  (unsigned int)status.hosted_fw_version_error);
            lv_label_set_text(s_ble_status_label, "状态: 待就绪");
            lv_label_set_text(s_ble_hint_label, "BLE follows hosted link");
        } else {
            lv_label_set_text(s_wifi_status_label, "状态: 未初始化");
            lv_label_set_text(s_wifi_link_label, "Link: --");
            lv_label_set_text(s_wifi_ap_label, "WiFi: --");
            lv_label_set_text(s_wifi_hint_label, "Hint: host NA");
            lv_label_set_text(s_ble_status_label, "状态: 未初始化");
            lv_label_set_text(s_ble_hint_label, "BLE host NA");
        }
        lv_obj_add_state(s_wifi_scan_btn, LV_STATE_DISABLED);
        if (s_wifi_list != NULL) {
            lv_obj_clean(s_wifi_list);
            lv_list_add_text(s_wifi_list, "C6 Link Failed");
        }
        return;
    }

    lv_label_set_text_fmt(s_wifi_status_label, "状态: %s",
                          wireless_transport_state_to_text(status.transport_state));
    lv_label_set_text_fmt(s_wifi_link_label, "Link: %s FW:%s RPC:%s",
                          status.cp_target_name[0] != '\0' ? status.cp_target_name : "--",
                          status.cp_fw_version[0] != '\0' ? status.cp_fw_version : "--",
                          status.rpc_metadata_ready ? "OK" : "WARN");
    lv_label_set_text_fmt(s_wifi_ap_label, "WiFi: %s AP=%" PRIu32 " up=%d init=%d",
                          wireless_wifi_state_to_text(status.wifi_state),
                          status.wifi_ap_count,
                          status.transport_up_received,
                          status.cp_init_received);
    lv_label_set_text_fmt(s_wifi_hint_label, "Hint: %s SSID=%s IP=%s evt=%" PRIi32,
                          status.last_hint,
                          status.current_ssid[0] != '\0' ? status.current_ssid : "--",
                          status.current_ip[0] != '\0' ? status.current_ip : "--",
                          status.hosted_last_event);
    lv_label_set_text_fmt(s_ble_status_label, "状态: %s", wireless_ble_state_to_text(status.ble_state));
    lv_label_set_text(s_ble_hint_label, "BLE Hosted 待接入");

    bool wifi_actions_ready = status.hosted_stack_available &&
                              status.wifi_feature_enabled &&
                              status.transport_confirmed &&
                              status.transport_state == TEST_WIRELESS_TRANSPORT_READY;

    if (wifi_actions_ready) {
        lv_obj_clear_state(s_wifi_scan_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_wifi_scan_btn, LV_STATE_DISABLED);
    }

    demo_refresh_wifi_ap_list(&status);
#else
    lv_label_set_text(s_wifi_status_label, "状态: 未开");
    lv_label_set_text(s_wifi_link_label, "Link: --");
    lv_label_set_text(s_wifi_ap_label, "WiFi: --");
    lv_label_set_text(s_wifi_hint_label, "开 CONFIG_EXAMPLE_ENABLE_TEST_WIRELESS");
    lv_label_set_text(s_ble_status_label, "状态: 未开");
    lv_label_set_text(s_ble_hint_label, "开 CONFIG_EXAMPLE_ENABLE_TEST_WIRELESS");
    lv_obj_add_state(s_wifi_scan_btn, LV_STATE_DISABLED);
    if (s_wifi_list != NULL) {
        lv_obj_clean(s_wifi_list);
        lv_list_add_text(s_wifi_list, "WiFi Disabled");
    }
#endif
}

static void demo_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    demo_update_touch_card();
    demo_update_rtc_card();
    demo_update_usb_card();
    demo_update_audio_card();
    demo_update_battery_card();
    demo_update_wireless_card();
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
    lv_label_set_text(subtitle, "显示/触摸 + RTC + USB Device CDC + 音频 + BAT + WiFi + BLE");

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

    lv_obj_t *battery_card = demo_create_card(row, "5. BAT", 488, 230);
    s_battery_status_label = lv_label_create(battery_card);
    s_battery_adc_label = lv_label_create(battery_card);
    s_battery_voltage_label = lv_label_create(battery_card);
    s_battery_percent_label = lv_label_create(battery_card);
    s_battery_hint_label = lv_label_create(battery_card);
    lv_obj_set_style_text_color(s_battery_hint_label, lv_color_hex(0x5E4C38), 0);

    lv_obj_t *wifi_card = demo_create_card(row, "6. WiFi", 488, 360);
    s_wifi_status_label = lv_label_create(wifi_card);
    s_wifi_link_label = lv_label_create(wifi_card);
    s_wifi_ap_label = lv_label_create(wifi_card);
    s_wifi_hint_label = lv_label_create(wifi_card);
    lv_obj_set_style_text_color(s_wifi_hint_label, lv_color_hex(0x5E4C38), 0);

    lv_obj_t *wifi_btn_row = lv_obj_create(wifi_card);
    lv_obj_set_width(wifi_btn_row, lv_pct(100));
    lv_obj_set_height(wifi_btn_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(wifi_btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_btn_row, 0, 0);
    lv_obj_set_style_pad_all(wifi_btn_row, 0, 0);
    lv_obj_set_style_pad_column(wifi_btn_row, 8, 0);
    lv_obj_set_layout(wifi_btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(wifi_btn_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_clear_flag(wifi_btn_row, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_scan_btn = lv_button_create(wifi_btn_row);
    lv_obj_set_size(s_wifi_scan_btn, 124, 44);
    lv_obj_add_event_cb(s_wifi_scan_btn, wireless_wifi_scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wifi_scan_label = lv_label_create(s_wifi_scan_btn);
    lv_label_set_text(wifi_scan_label, "WiFi Scan");
    lv_obj_center(wifi_scan_label);

    s_wifi_list = lv_list_create(wifi_card);
    lv_obj_set_size(s_wifi_list, lv_pct(100), 150);
    lv_obj_set_style_pad_row(s_wifi_list, 6, 0);
    lv_list_add_text(s_wifi_list, "Tap Scan");

    lv_obj_t *ble_card = demo_create_card(row, "7. BLE", 488, 180);
    s_ble_status_label = lv_label_create(ble_card);
    s_ble_hint_label = lv_label_create(ble_card);
    lv_obj_set_style_text_color(s_ble_hint_label, lv_color_hex(0x5E4C38), 0);
    lv_label_set_text(s_ble_status_label, "状态: host NA");
    lv_label_set_text(s_ble_hint_label, "BLE Not Ready");

    lv_obj_t *top_layer = lv_display_get_layer_top(disp);
    s_wifi_dialog_mask = lv_obj_create(top_layer);
    lv_obj_remove_style_all(s_wifi_dialog_mask);
    lv_obj_set_size(s_wifi_dialog_mask, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_wifi_dialog_mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_wifi_dialog_mask, LV_OPA_50, 0);
    lv_obj_add_flag(s_wifi_dialog_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wifi_dialog_mask, LV_OBJ_FLAG_CLICKABLE);

    s_wifi_dialog = lv_obj_create(s_wifi_dialog_mask);
    lv_obj_set_size(s_wifi_dialog, 520, 250);
    lv_obj_center(s_wifi_dialog);
    lv_obj_set_style_radius(s_wifi_dialog, 20, 0);
    lv_obj_set_style_pad_all(s_wifi_dialog, 16, 0);
    lv_obj_set_style_pad_row(s_wifi_dialog, 10, 0);
    lv_obj_set_layout(s_wifi_dialog, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_wifi_dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_wifi_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dialog_title = lv_label_create(s_wifi_dialog);
    lv_label_set_text(dialog_title, "WiFi Join");
    lv_obj_set_style_text_font(dialog_title, demo_get_ui_font(), 0);

    s_wifi_dialog_ssid_label = lv_label_create(s_wifi_dialog);
    lv_label_set_text(s_wifi_dialog_ssid_label, "SSID: --");

    s_wifi_password_ta = lv_textarea_create(s_wifi_dialog);
    lv_obj_set_width(s_wifi_password_ta, lv_pct(100));
    lv_textarea_set_one_line(s_wifi_password_ta, true);
    lv_textarea_set_password_mode(s_wifi_password_ta, true);
    lv_textarea_set_placeholder_text(s_wifi_password_ta, "Password");
    lv_obj_add_event_cb(s_wifi_password_ta, wifi_password_ta_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *dialog_btn_row = lv_obj_create(s_wifi_dialog);
    lv_obj_set_width(dialog_btn_row, lv_pct(100));
    lv_obj_set_height(dialog_btn_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(dialog_btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dialog_btn_row, 0, 0);
    lv_obj_set_style_pad_all(dialog_btn_row, 0, 0);
    lv_obj_set_style_pad_column(dialog_btn_row, 10, 0);
    lv_obj_set_layout(dialog_btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dialog_btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(dialog_btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *connect_btn = lv_button_create(dialog_btn_row);
    lv_obj_set_size(connect_btn, 110, 42);
    lv_obj_add_event_cb(connect_btn, wifi_dialog_connect_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *connect_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_center(connect_label);

    lv_obj_t *cancel_btn = lv_button_create(dialog_btn_row);
    lv_obj_set_size(cancel_btn, 110, 42);
    lv_obj_add_event_cb(cancel_btn, wifi_dialog_cancel_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);

    s_wifi_keyboard = lv_keyboard_create(top_layer);
    lv_obj_set_size(s_wifi_keyboard, lv_pct(100), 220);
    lv_obj_align(s_wifi_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_wifi_keyboard, NULL);
    lv_obj_add_event_cb(s_wifi_keyboard, wifi_keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);

    demo_update_touch_card();
    demo_update_rtc_card();
    demo_update_usb_card();
    demo_update_audio_card();
    demo_update_battery_card();
    demo_update_wireless_card();
    lv_timer_create(demo_ui_timer_cb, 250, NULL);
}
