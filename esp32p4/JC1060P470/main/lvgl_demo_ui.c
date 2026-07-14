/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "lvgl.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "display_font.h"
#include "display_touch.h"
#include "esp_cache.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl_demo_font_patch.h"
#include "test_audio.h"
#include "test_battery.h"
#include "test_rtc.h"
#include "test_usb.h"
#include "test_usb_role.h"
#include "test_uvc.h"
#include "test_wireless.h"
#include "test_config.h"

static const char *TAG = "demo_ui";

typedef void (*demo_ui_progress_cb_t)(const char *stage);

static void demo_build_progress(demo_ui_progress_cb_t progress_cb, const char *stage)
{
    if (progress_cb != NULL) {
        progress_cb(stage);
    }
}

typedef struct {
    uint32_t touch_count;
    uint32_t last_touch_ms;
    lv_point_t last_point;
    bool touch_active;
} demo_touch_state_t;

typedef enum {
    DEMO_WIFI_LIST_VIEW_UNKNOWN,
    DEMO_WIFI_LIST_VIEW_NOT_READY,
    DEMO_WIFI_LIST_VIEW_WIFI_NOT_READY,
    DEMO_WIFI_LIST_VIEW_C6_LINK_FAILED,
    DEMO_WIFI_LIST_VIEW_TAP_SCAN,
    DEMO_WIFI_LIST_VIEW_SCANNING,
    DEMO_WIFI_LIST_VIEW_READ_FAIL,
    DEMO_WIFI_LIST_VIEW_EMPTY,
    DEMO_WIFI_LIST_VIEW_DISABLED,
    DEMO_WIFI_LIST_VIEW_RESULTS,
} demo_wifi_list_view_t;

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
static lv_obj_t *s_usb_switch_btn;
static lv_obj_t *s_uvc_enter_btn;

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
static lv_obj_t *s_ble_name_label;
static lv_obj_t *s_ble_link_label;
static lv_obj_t *s_ble_value_label;
static lv_obj_t *s_ble_hint_label;
static lv_obj_t *s_ble_adv_btn;
static lv_obj_t *s_ble_adv_btn_label;
static lv_obj_t *s_font_status_label;
static lv_obj_t *s_font_file_label;
static lv_obj_t *s_font_size_label;
static lv_obj_t *s_font_glyph_label;
static lv_obj_t *s_font_touch_label;
static lv_obj_t *s_font_sample_area;
static lv_obj_t *s_font_sample_label;
static lv_obj_t *s_wifi_dialog_mask;
static lv_obj_t *s_wifi_dialog;
static lv_obj_t *s_wifi_dialog_ssid_label;
static lv_obj_t *s_wifi_password_ta;
static lv_obj_t *s_wifi_keyboard;

static lv_obj_t *s_main_screen;
static lv_obj_t *s_uvc_screen;
static lv_obj_t *s_uvc_status_label;
static lv_obj_t *s_uvc_detail_label;
static lv_obj_t *s_uvc_resolution_dropdown;
static lv_obj_t *s_uvc_preview_image;
static lv_timer_t *s_uvc_timer;
static uint8_t *s_uvc_preview_image_buf;
static lv_image_dsc_t s_uvc_preview_dsc;
static uint32_t s_uvc_preview_frame_id;
static size_t s_uvc_resolution_count;
static test_uvc_resolution_t s_uvc_resolutions[TEST_UVC_FRAME_INFO_MAX];

#define DEMO_UVC_PREVIEW_SIZE (TEST_UVC_PREVIEW_H_RES * TEST_UVC_PREVIEW_V_RES * 2U)

static demo_touch_state_t s_touch_state;
static test_wireless_ap_record_t s_wifi_ap_records[TEST_WIRELESS_SCAN_RESULT_MAX];
static size_t s_wifi_ap_record_count;
static uint32_t s_wifi_scan_generation;
static demo_wifi_list_view_t s_wifi_list_view = DEMO_WIFI_LIST_VIEW_UNKNOWN;
static char s_selected_wifi_ssid[TEST_WIRELESS_SSID_MAX_LEN + 1];
static uint32_t s_ui_label_update_count;
static uint32_t s_ui_list_rebuild_count;
static uint32_t s_ui_update_report_start_ms;
static uint16_t s_font_sample_size_px = 22;
static uint16_t s_font_pinch_base_size_px = 22;
static float s_font_pinch_base_distance;
static bool s_font_pinch_active;

#define WIFI_KB_CTRL_KEY (LV_KEYBOARD_CTRL_BUTTON_FLAGS)
#define WIFI_KB_CHECKED_KEY (LV_BUTTONMATRIX_CTRL_CHECKED)

static const char * const s_wifi_keyboard_lower_map[] = {
    "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "Bksp", "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", "Enter", "\n",
    "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    "Hide", "Left", " ", "Right", "OK", ""
};

static const lv_buttonmatrix_ctrl_t s_wifi_keyboard_lower_ctrl_map[] = {
    WIFI_KB_CTRL_KEY | 5, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, WIFI_KB_CHECKED_KEY | 7,
    WIFI_KB_CTRL_KEY | 6, 3, 3, 3, 3, 3, 3, 3, 3, 3, WIFI_KB_CHECKED_KEY | 7,
    WIFI_KB_CHECKED_KEY | 1, WIFI_KB_CHECKED_KEY | 1, 1, 1, 1, 1, 1, 1, 1,
    WIFI_KB_CHECKED_KEY | 1, WIFI_KB_CHECKED_KEY | 1, WIFI_KB_CHECKED_KEY | 1,
    WIFI_KB_CTRL_KEY | 2, WIFI_KB_CHECKED_KEY | 2, 6, WIFI_KB_CHECKED_KEY | 2, WIFI_KB_CTRL_KEY | 2
};

static const char * const s_wifi_keyboard_upper_map[] = {
    "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "Bksp", "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", "Enter", "\n",
    "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
    "Hide", "Left", " ", "Right", "OK", ""
};

static const lv_buttonmatrix_ctrl_t s_wifi_keyboard_upper_ctrl_map[] = {
    WIFI_KB_CTRL_KEY | 5, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, WIFI_KB_CHECKED_KEY | 7,
    WIFI_KB_CTRL_KEY | 6, 3, 3, 3, 3, 3, 3, 3, 3, 3, WIFI_KB_CHECKED_KEY | 7,
    WIFI_KB_CHECKED_KEY | 1, WIFI_KB_CHECKED_KEY | 1, 1, 1, 1, 1, 1, 1, 1,
    WIFI_KB_CHECKED_KEY | 1, WIFI_KB_CHECKED_KEY | 1, WIFI_KB_CHECKED_KEY | 1,
    WIFI_KB_CTRL_KEY | 2, WIFI_KB_CHECKED_KEY | 2, 6, WIFI_KB_CHECKED_KEY | 2, WIFI_KB_CTRL_KEY | 2
};

static const char * const s_wifi_keyboard_special_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "Bksp", "\n",
    "abc", "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
    "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
    "Hide", "Left", " ", "Right", "OK", ""
};

static const lv_buttonmatrix_ctrl_t s_wifi_keyboard_special_ctrl_map[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, WIFI_KB_CHECKED_KEY | 2,
    WIFI_KB_CTRL_KEY | 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    WIFI_KB_CTRL_KEY | 2, WIFI_KB_CHECKED_KEY | 2, 6, WIFI_KB_CHECKED_KEY | 2, WIFI_KB_CTRL_KEY | 2
};

static const lv_font_t *demo_get_role_font(display_font_role_t role)
{
    return display_font_get_role_font(role);
}

static void demo_set_text_role(lv_obj_t *obj, display_font_role_t role)
{
    lv_obj_set_style_text_font(obj, demo_get_role_font(role), 0);
}

static void demo_config_label(lv_obj_t *label, display_font_role_t role)
{
    demo_set_text_role(label, role);
}

static void demo_config_list_label(lv_obj_t *label, display_font_role_t role)
{
    demo_set_text_role(label, role);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
}

static bool demo_label_set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return false;
    }

    const char *current = lv_label_get_text(label);
    if (current != NULL && strcmp(current, text) == 0) {
        return false;
    }

    lv_label_set_text(label, text);
    s_ui_label_update_count++;
    return true;
}

static bool demo_label_set_text_fmt_if_changed(lv_obj_t *label, const char *fmt, ...)
{
    char text[192];
    va_list args;

    va_start(args, fmt);
    int len = vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    if (len < 0) {
        return false;
    }

    text[sizeof(text) - 1] = '\0';
    return demo_label_set_text_if_changed(label, text);
}

static void demo_obj_set_disabled(lv_obj_t *obj, bool disabled)
{
    if (obj == NULL || lv_obj_has_state(obj, LV_STATE_DISABLED) == disabled) {
        return;
    }

    if (disabled) {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(obj, LV_STATE_DISABLED);
    }
}

static lv_obj_t *demo_wifi_list_add_text(const char *text)
{
    lv_obj_t *item = lv_label_create(s_wifi_list);
    lv_label_set_text(item, text);
    demo_config_list_label(item, DISPLAY_FONT_ROLE_SMALL);
    return item;
}

static lv_obj_t *demo_wifi_list_add_button(const char *text)
{
    lv_obj_t *item = lv_label_create(s_wifi_list);
    lv_label_set_text(item, text);
    demo_config_list_label(item, DISPLAY_FONT_ROLE_SMALL);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_ver(item, 4, 0);
    return item;
}

static void demo_ui_report_updates_if_ready(void)
{
    uint32_t now_ms = lv_tick_get();
    if (s_ui_update_report_start_ms == 0) {
        s_ui_update_report_start_ms = now_ms;
        return;
    }

    if (lv_tick_elaps(s_ui_update_report_start_ms) < 1000) {
        return;
    }

    lv_mem_monitor_t mon = {0};
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "UI updates: labels=%" PRIu32 " list_rebuilds=%" PRIu32
             " lv_mem_free=%u biggest=%u used=%u%% frag=%u%%",
             s_ui_label_update_count,
             s_ui_list_rebuild_count,
             (unsigned int)mon.free_size,
             (unsigned int)mon.free_biggest_size,
             (unsigned int)mon.used_pct,
             (unsigned int)mon.frag_pct);

    s_ui_label_update_count = 0;
    s_ui_list_rebuild_count = 0;
    s_ui_update_report_start_ms = now_ms;
}

static bool demo_utf8_sequence_valid(const uint8_t *text, size_t len, size_t *advance)
{
    uint32_t codepoint = 0;
    uint8_t first = text[0];

    if (first < 0x80) {
        *advance = 1;
        return true;
    }

    if (first >= 0xC2 && first <= 0xDF) {
        if (len < 2 || (text[1] & 0xC0) != 0x80) {
            return false;
        }
        *advance = 2;
        return true;
    }

    if (first >= 0xE0 && first <= 0xEF) {
        if (len < 3 || (text[1] & 0xC0) != 0x80 || (text[2] & 0xC0) != 0x80) {
            return false;
        }
        if ((first == 0xE0 && text[1] < 0xA0) || (first == 0xED && text[1] >= 0xA0)) {
            return false;
        }
        *advance = 3;
        return true;
    }

    if (first >= 0xF0 && first <= 0xF4) {
        if (len < 4 || (text[1] & 0xC0) != 0x80 || (text[2] & 0xC0) != 0x80 || (text[3] & 0xC0) != 0x80) {
            return false;
        }
        if ((first == 0xF0 && text[1] < 0x90) || (first == 0xF4 && text[1] > 0x8F)) {
            return false;
        }
        codepoint = ((uint32_t)(first & 0x07) << 18) |
                    ((uint32_t)(text[1] & 0x3F) << 12) |
                    ((uint32_t)(text[2] & 0x3F) << 6) |
                    (uint32_t)(text[3] & 0x3F);
        if (codepoint > 0x10FFFF) {
            return false;
        }
        *advance = 4;
        return true;
    }

    return false;
}

static bool demo_utf8_string_valid(const char *text)
{
    const uint8_t *bytes = (const uint8_t *)text;
    size_t len = strlen(text);
    size_t offset = 0;

    while (offset < len) {
        size_t advance = 0;
        if (!demo_utf8_sequence_valid(&bytes[offset], len - offset, &advance)) {
            return false;
        }
        offset += advance;
    }

    return true;
}

static const char *demo_wifi_ssid_display_text(const char *ssid, char *buf, size_t buf_size)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return "<Hidden>";
    }

    if (demo_utf8_string_valid(ssid)) {
        return ssid;
    }

    if (buf_size > 0) {
        snprintf(buf, buf_size, "<SSID HEX:%02X%02X%02X%02X>",
                 (unsigned char)ssid[0],
                 (unsigned char)ssid[1],
                 (unsigned char)ssid[2],
                 (unsigned char)ssid[3]);
    }
    return buf;
}

static lv_obj_t *demo_create_card(lv_obj_t *parent, const char *title, lv_coord_t width, lv_coord_t height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, width, height);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x1E3A5F), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xF8F2E7), 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    demo_config_label(title_label, DISPLAY_FONT_ROLE_CARD_TITLE);
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

static void usb_switch_btn_cb(lv_event_t *e)
{
    (void)e;
    test_usb_role_t current = test_usb_role_current();
    if (current == TEST_USB_ROLE_CDC_DEVICE) {
        test_usb_role_switch(TEST_USB_ROLE_UVC_HOST);
    } else if (current == TEST_USB_ROLE_UVC_HOST) {
        test_usb_role_switch(TEST_USB_ROLE_CDC_DEVICE);
    } else {
        test_usb_role_switch(TEST_USB_ROLE_CDC_DEVICE);
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
        return "INIT";
    case TEST_WIRELESS_TRANSPORT_NOT_AVAILABLE:
        return "host NA";
    case TEST_WIRELESS_TRANSPORT_IDLE:
        return "IDLE";
    case TEST_WIRELESS_TRANSPORT_CONNECTING:
        return "LINKING";
    case TEST_WIRELESS_TRANSPORT_READY:
        return "READY";
    case TEST_WIRELESS_TRANSPORT_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

static const char *wireless_wifi_state_to_text(test_wireless_wifi_state_t state)
{
    switch (state) {
    case TEST_WIRELESS_WIFI_UNINITIALIZED:
        return "INIT";
    case TEST_WIRELESS_WIFI_NOT_AVAILABLE:
        return "host NA";
    case TEST_WIRELESS_WIFI_IDLE:
        return "IDLE";
    case TEST_WIRELESS_WIFI_SCANNING:
        return "Scan";
    case TEST_WIRELESS_WIFI_CONNECTING:
        return "Join";
    case TEST_WIRELESS_WIFI_CONNECTED:
        return "CONNECTED";
    case TEST_WIRELESS_WIFI_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

static const char *wireless_ble_state_to_text(test_wireless_ble_state_t state)
{
    switch (state) {
    case TEST_WIRELESS_BLE_UNINITIALIZED:
        return "INIT";
    case TEST_WIRELESS_BLE_NOT_AVAILABLE:
        return "host NA";
    case TEST_WIRELESS_BLE_IDLE:
        return "IDLE";
    case TEST_WIRELESS_BLE_SCANNING:
        return "Scan";
    case TEST_WIRELESS_BLE_ADVERTISING:
        return "Adv On";
    case TEST_WIRELESS_BLE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
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

static void wireless_ble_adv_btn_cb(lv_event_t *e)
{
    (void)e;

#if CONFIG_EXAMPLE_ENABLE_TEST_WIRELESS && CONFIG_EXAMPLE_WIRELESS_ENABLE_BLE
    test_wireless_status_t status = {0};
    esp_err_t ret = test_wireless_get_status(&status);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ble status read failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = test_wireless_set_ble_advertising(!status.ble_advertising_enabled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ble advertising request failed: %s", esp_err_to_name(ret));
    }
#endif
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

static void wifi_keyboard_preprocess_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED || s_wifi_keyboard == NULL) {
        return;
    }

    uint32_t btn_id = lv_keyboard_get_selected_button(s_wifi_keyboard);
    if (btn_id == LV_BUTTONMATRIX_BUTTON_NONE) {
        return;
    }

    const char *text = lv_keyboard_get_button_text(s_wifi_keyboard, btn_id);
    if (text == NULL) {
        return;
    }

    if (strcmp(text, "Bksp") == 0) {
        if (s_wifi_password_ta != NULL) {
            lv_textarea_delete_char(s_wifi_password_ta);
        }
        lv_event_stop_processing(e);
    } else if (strcmp(text, "Left") == 0) {
        if (s_wifi_password_ta != NULL) {
            lv_textarea_cursor_left(s_wifi_password_ta);
        }
        lv_event_stop_processing(e);
    } else if (strcmp(text, "Right") == 0) {
        if (s_wifi_password_ta != NULL) {
            lv_textarea_cursor_right(s_wifi_password_ta);
        }
        lv_event_stop_processing(e);
    } else if (strcmp(text, "OK") == 0 || strcmp(text, "Enter") == 0) {
        wifi_dialog_connect_btn_cb(e);
        lv_event_stop_processing(e);
    } else if (strcmp(text, "Hide") == 0) {
        wifi_dialog_close();
        lv_event_stop_processing(e);
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

    demo_label_set_text_fmt_if_changed(s_touch_status_label, "状态: %s", s_touch_state.touch_active ? "按下中" : "待触摸");
    demo_label_set_text_fmt_if_changed(s_touch_count_label, "点击次数: %" PRIu32, s_touch_state.touch_count);
    demo_label_set_text_fmt_if_changed(s_touch_point_label, "最近坐标: (%" PRId32 ", %" PRId32 ")",
                                       s_touch_state.last_point.x, s_touch_state.last_point.y);

    if (s_touch_state.last_touch_ms == 0) {
        demo_label_set_text_if_changed(s_touch_hint_label, "在下方触摸区点击，验证显示与触摸联动");
    } else {
        demo_label_set_text_fmt_if_changed(s_touch_hint_label, "最近点击: %" PRIu32 " ms", s_touch_state.last_touch_ms);
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
        demo_label_set_text_if_changed(s_rtc_status_label, "状态: 未初始化");
        demo_label_set_text_if_changed(s_rtc_time_label, "时间: --");
        demo_label_set_text_if_changed(s_rtc_sync_label, "同步: --");
        return;
    }

    if (status.time_valid) {
        demo_label_set_text_if_changed(s_rtc_status_label, "状态: 运行中");
        demo_label_set_text_fmt_if_changed(s_rtc_time_label, "时间: %04u-%02u-%02u %02u:%02u:%02u",
                                           status.year, status.month, status.mday,
                                           status.hour, status.min, status.sec);
    } else {
        demo_label_set_text_fmt_if_changed(s_rtc_status_label, "状态: 错误(0x%x)", (unsigned int)status.last_error);
        demo_label_set_text_if_changed(s_rtc_time_label, "时间: 无有效值");
    }

    demo_label_set_text_fmt_if_changed(s_rtc_sync_label, "同步: %s",
                                       status.synced_from_build_time ? "曾从固件构建时间写入" : "保持 RTC 原值");
}

static void demo_update_usb_card(void)
{
    if (s_usb_status_label == NULL) {
        return;
    }

    test_usb_role_status_t status = {0};
    esp_err_t ret = test_usb_role_get_status(&status);
    if (ret != ESP_OK) {
        demo_label_set_text_if_changed(s_usb_status_label, "状态: 未初始化");
        demo_label_set_text_if_changed(s_usb_line_label, "线路: --");
        demo_label_set_text_if_changed(s_usb_stats_label, "收发: --");
        demo_label_set_text_if_changed(s_usb_hint_label, "--");
        return;
    }

    /* Status line: current role + switch progress */
    if (status.switch_in_progress) {
        demo_label_set_text_if_changed(s_usb_status_label, "状态: 切换中...");
    } else if (status.last_switch_error != ESP_OK && status.current_role == TEST_USB_ROLE_NONE) {
        demo_label_set_text_fmt_if_changed(s_usb_status_label, "状态: 切换失败(0x%x)", (unsigned int)status.last_switch_error);
    } else {
        const char *role_text = "空闲";
        if (status.current_role == TEST_USB_ROLE_CDC_DEVICE) {
            role_text = status.cdc_attached ? "CDC 已连接主机" : "CDC 等待枚举";
        } else if (status.current_role == TEST_USB_ROLE_UVC_HOST) {
            role_text = status.uvc_device_connected ? "UVC 设备已连接" : "UVC 等待设备";
        }
        demo_label_set_text_if_changed(s_usb_status_label, role_text);
    }

    /* Second line: role-specific details */
    if (status.current_role == TEST_USB_ROLE_CDC_DEVICE) {
        test_usb_status_t cdc = {0};
        if (test_usb_get_status(&cdc) == ESP_OK && cdc.initialized) {
            demo_label_set_text_fmt_if_changed(s_usb_line_label, "DTR=%d RTS=%d 收=%uB 发=%uB",
                                               cdc.dtr, cdc.rts,
                                               (unsigned int)cdc.last_rx_size, (unsigned int)cdc.last_tx_size);
            demo_label_set_text_fmt_if_changed(s_usb_stats_label, "RX %" PRIu32 " 包/%" PRIu32 " B  TX %" PRIu32 " 包/%" PRIu32 " B",
                                               cdc.rx_packets, cdc.rx_bytes, cdc.tx_packets, cdc.tx_bytes);
        } else {
            demo_label_set_text_if_changed(s_usb_line_label, "线路: --");
            demo_label_set_text_if_changed(s_usb_stats_label, "收发: --");
        }
        demo_label_set_text_if_changed(s_usb_hint_label, "连接 HIGH USB 口，用串口工具发送数据验证 CDC 回显");
    } else if (status.current_role == TEST_USB_ROLE_UVC_HOST) {
        if (status.uvc_format_negotiated) {
            demo_label_set_text_fmt_if_changed(s_usb_line_label, "协商: %ux%u@%.1fFPS",
                                               (unsigned)status.uvc_negotiated_h_res,
                                               (unsigned)status.uvc_negotiated_v_res,
                                               status.uvc_negotiated_fps);
        } else {
            demo_label_set_text_if_changed(s_usb_line_label, "协商: 等待设备描述符");
        }
        demo_label_set_text_fmt_if_changed(s_usb_stats_label, "已解码 %" PRIu32 " 帧  错误 %" PRIu32,
                                           status.uvc_frames_decoded, status.uvc_decode_errors);
        demo_label_set_text_if_changed(s_usb_hint_label, "连接 UVC 摄像头到 HIGH USB 口");
    } else {
        demo_label_set_text_if_changed(s_usb_line_label, "线路: --");
        demo_label_set_text_if_changed(s_usb_stats_label, "收发: --");
        demo_label_set_text_if_changed(s_usb_hint_label, "点击下方按钮切换 USB 角色");
    }

    /* Update switch button label based on current role */
    if (s_usb_switch_btn != NULL) {
        lv_obj_t *btn_label = lv_obj_get_child(s_usb_switch_btn, 0);
        if (btn_label != NULL) {
            const char *btn_text = "切换到 CDC";
            if (status.current_role == TEST_USB_ROLE_CDC_DEVICE) {
                btn_text = "切换到 UVC";
            }
            demo_label_set_text_if_changed(btn_label, btn_text);
        }
        lv_obj_clear_state(s_usb_switch_btn, LV_STATE_DISABLED);
        if (status.switch_in_progress) {
            lv_obj_add_state(s_usb_switch_btn, LV_STATE_DISABLED);
        }
    }
}

static void demo_update_audio_card(void)
{
    if (s_audio_status_label == NULL) {
        return;
    }

    test_audio_status_t status = {0};
    esp_err_t ret = test_audio_get_status(&status);
    if (ret != ESP_OK || !status.initialized) {
        demo_label_set_text_if_changed(s_audio_status_label, "状态: 未初始化");
        demo_label_set_text_if_changed(s_audio_size_label, "录音数据: --");
        demo_label_set_text_if_changed(s_audio_peak_label, "峰值: --");
        demo_label_set_text_if_changed(s_audio_input_label, "输入电平: --");
        return;
    }

    if (status.state == TEST_AUDIO_STATE_ERROR) {
        demo_label_set_text_fmt_if_changed(s_audio_status_label, "状态: 错误(0x%x)", (unsigned int)status.last_error);
    } else {
        demo_label_set_text_fmt_if_changed(s_audio_status_label, "状态: %s", audio_state_to_text(status.state));
    }

    demo_label_set_text_fmt_if_changed(s_audio_size_label, "录音数据: %u B", (unsigned int)status.last_record_size);
    demo_label_set_text_fmt_if_changed(s_audio_peak_label, "峰值: %u", (unsigned int)status.last_record_peak);
    demo_label_set_text_fmt_if_changed(s_audio_input_label, "输入电平: %s", audio_input_level_text(status.last_record_peak,
                                                                                                 status.last_record_nonzero_count));

    bool can_start_new = (status.state == TEST_AUDIO_STATE_IDLE ||
                          status.state == TEST_AUDIO_STATE_RECORDED ||
                          status.state == TEST_AUDIO_STATE_ERROR);
    bool can_play_record = (status.state == TEST_AUDIO_STATE_RECORDED && status.last_record_size > 0);

    if (can_start_new) {
        demo_obj_set_disabled(s_audio_music_btn, false);
        demo_obj_set_disabled(s_audio_record_btn, false);
    } else {
        demo_obj_set_disabled(s_audio_music_btn, true);
        demo_obj_set_disabled(s_audio_record_btn, true);
    }

    if (can_play_record) {
        demo_obj_set_disabled(s_audio_play_btn, false);
    } else {
        demo_obj_set_disabled(s_audio_play_btn, true);
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
        demo_label_set_text_if_changed(s_battery_status_label, "状态: 初始化中");
        demo_label_set_text_if_changed(s_battery_adc_label, "ADC值: --");
        demo_label_set_text_if_changed(s_battery_voltage_label, "BAT值: --");
        demo_label_set_text_if_changed(s_battery_percent_label, "BAT%: --");
        demo_label_set_text_if_changed(s_battery_hint_label, "GPIO52 BAT 点");
        return;
    }

    esp_err_t ret = test_battery_run();

    if (!status->sample_valid) {
        demo_label_set_text_fmt_if_changed(s_battery_status_label, "状态: %s(0x%x)",
                                           ret == ESP_OK ? "采样中" : "错误",
                                           (unsigned int)status->last_error);
        demo_label_set_text_if_changed(s_battery_adc_label, "ADC值: --");
        demo_label_set_text_if_changed(s_battery_voltage_label, "BAT值: --");
        demo_label_set_text_if_changed(s_battery_percent_label, "BAT%: --");
    } else {
        demo_label_set_text_fmt_if_changed(s_battery_status_label, "状态: %s", status->calibration_enabled ? "运行中(CAL)" : "运行中(RAW)");
        demo_label_set_text_fmt_if_changed(s_battery_adc_label, "ADC值: %d.%03d V",
                                           status->adc_voltage_mv / 1000, status->adc_voltage_mv % 1000);
        demo_label_set_text_fmt_if_changed(s_battery_voltage_label, "BAT值: %d.%03d V",
                                           status->battery_voltage_mv / 1000, status->battery_voltage_mv % 1000);
        demo_label_set_text_fmt_if_changed(s_battery_percent_label, "BAT%%: %d%%", status->battery_percent);
    }

    demo_label_set_text_fmt_if_changed(s_battery_hint_label, "次数: %" PRIu32, status->sample_count);
#else
    demo_label_set_text_if_changed(s_battery_status_label, "状态: 未开");
    demo_label_set_text_if_changed(s_battery_adc_label, "ADC值: --");
    demo_label_set_text_if_changed(s_battery_voltage_label, "BAT值: --");
    demo_label_set_text_if_changed(s_battery_percent_label, "BAT%: --");
    demo_label_set_text_if_changed(s_battery_hint_label, "开 CONFIG_EXAMPLE_ENABLE_TEST_BATTERY");
#endif
}

static void demo_wifi_list_set_message(demo_wifi_list_view_t view, const char *message, uint32_t generation)
{
    if (s_wifi_list_view == view && s_wifi_scan_generation == generation) {
        return;
    }

    lv_obj_clean(s_wifi_list);
    demo_wifi_list_add_text(message);
    s_wifi_list_view = view;
    s_wifi_scan_generation = generation;
    s_wifi_ap_record_count = 0;
    s_ui_list_rebuild_count++;
}

static void demo_refresh_wifi_ap_list(const test_wireless_status_t *status)
{
    if (s_wifi_list == NULL || status == NULL) {
        return;
    }

    if (!status->transport_confirmed) {
        demo_wifi_list_set_message(DEMO_WIFI_LIST_VIEW_NOT_READY, "C6 Link Not Ready", status->wifi_scan_generation);
        return;
    }

    if (!status->wifi_scan_results_valid && status->wifi_state != TEST_WIRELESS_WIFI_SCANNING) {
        demo_wifi_list_set_message(DEMO_WIFI_LIST_VIEW_TAP_SCAN, "Tap Scan", status->wifi_scan_generation);
        return;
    }

    if (status->wifi_state == TEST_WIRELESS_WIFI_SCANNING) {
        demo_wifi_list_set_message(DEMO_WIFI_LIST_VIEW_SCANNING, "Scanning...", status->wifi_scan_generation);
        return;
    }

    if (s_wifi_list_view == DEMO_WIFI_LIST_VIEW_RESULTS &&
        status->wifi_scan_generation == s_wifi_scan_generation) {
        return;
    }

    size_t ap_count = 0;
    esp_err_t ret = test_wireless_get_scan_results(s_wifi_ap_records,
                                                   TEST_WIRELESS_SCAN_RESULT_MAX,
                                                   &ap_count);
    if (ret != ESP_OK) {
        demo_wifi_list_set_message(DEMO_WIFI_LIST_VIEW_READ_FAIL, "Scan Read Fail", status->wifi_scan_generation);
        return;
    }

    lv_obj_clean(s_wifi_list);
    s_wifi_ap_record_count = ap_count;
    s_wifi_scan_generation = status->wifi_scan_generation;
    s_wifi_list_view = ap_count == 0 ? DEMO_WIFI_LIST_VIEW_EMPTY : DEMO_WIFI_LIST_VIEW_RESULTS;
    s_ui_list_rebuild_count++;

    if (ap_count == 0) {
        demo_wifi_list_add_text("Scan Done, 0 AP");
        return;
    }

    for (size_t i = 0; i < ap_count; i++) {
        char ssid_display[32];
        char item_text[96];
        const char *ssid = demo_wifi_ssid_display_text(s_wifi_ap_records[i].ssid,
                                                       ssid_display,
                                                       sizeof(ssid_display));
        snprintf(item_text, sizeof(item_text), "%s %ddBm %s",
                 ssid, s_wifi_ap_records[i].rssi, wireless_wifi_security_text(s_wifi_ap_records[i].secured));
        lv_obj_t *item = demo_wifi_list_add_button(item_text);
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
    char ssid_display[32];
    lv_label_set_text_fmt(s_wifi_dialog_ssid_label, "SSID: %s",
                          demo_wifi_ssid_display_text(ssid, ssid_display, sizeof(ssid_display)));
    lv_textarea_set_text(s_wifi_password_ta, "");
    lv_obj_clear_flag(s_wifi_dialog_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_wifi_keyboard, s_wifi_password_ta);
    lv_obj_move_foreground(s_wifi_dialog_mask);
    lv_obj_move_foreground(s_wifi_dialog);
    lv_obj_move_foreground(s_wifi_keyboard);
    lv_obj_add_state(s_wifi_password_ta, LV_STATE_FOCUSED);
}

static void demo_set_ble_adv_button(bool enabled, bool advertising)
{
    if (s_ble_adv_btn == NULL || s_ble_adv_btn_label == NULL) {
        return;
    }

    demo_label_set_text_if_changed(s_ble_adv_btn_label, advertising ? "Adv Off" : "Adv On");
    demo_obj_set_disabled(s_ble_adv_btn, !enabled);
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
        demo_label_set_text_if_changed(s_wifi_status_label, "状态: 未初始化");
        demo_label_set_text_if_changed(s_wifi_link_label, "Link: --");
        demo_label_set_text_if_changed(s_wifi_ap_label, "WiFi: --");
        demo_label_set_text_if_changed(s_wifi_hint_label, "Hint: wireless NA");
        demo_label_set_text_if_changed(s_ble_status_label, "状态: 未初始化");
        demo_label_set_text_if_changed(s_ble_name_label, "Name: --");
        demo_label_set_text_if_changed(s_ble_link_label, "Link: --");
        demo_label_set_text_if_changed(s_ble_value_label, "GATT值: --");
        demo_label_set_text_if_changed(s_ble_hint_label, "BLE wireless NA");
        demo_set_ble_adv_button(false, false);
        demo_obj_set_disabled(s_wifi_scan_btn, true);
        if (s_wifi_list != NULL) {
            demo_wifi_list_set_message(DEMO_WIFI_LIST_VIEW_WIFI_NOT_READY, "WiFi Not Ready", 0);
        }
        return;
    }

    if (!status.initialized) {
        if (status.init_attempted) {
            demo_label_set_text_fmt_if_changed(s_wifi_status_label, "状态: %s(0x%x)",
                                               wireless_transport_state_to_text(status.transport_state),
                                               (unsigned int)status.last_error);
            demo_label_set_text_fmt_if_changed(s_wifi_link_label, "Link: %s FW:%s RPC:%s",
                                               status.cp_target_name[0] != '\0' ? status.cp_target_name : "--",
                                               status.cp_fw_version[0] != '\0' ? status.cp_fw_version : "--",
                                               status.rpc_metadata_ready ? "OK" : "WARN");
            demo_label_set_text_fmt_if_changed(s_wifi_ap_label, "WiFi: %s",
                                               wireless_wifi_state_to_text(status.wifi_state));
            demo_label_set_text_fmt_if_changed(s_wifi_hint_label, "Hint: %s err=0x%x",
                                               status.last_hint,
                                               (unsigned int)status.last_error);
            demo_label_set_text_if_changed(s_ble_status_label, "状态: 待就绪");
            demo_label_set_text_if_changed(s_ble_name_label, "Name: --");
            demo_label_set_text_if_changed(s_ble_link_label, "Link: hosted not ready");
            demo_label_set_text_if_changed(s_ble_value_label, "GATT值: --");
            demo_label_set_text_if_changed(s_ble_hint_label, "BLE follows hosted link");
        } else {
            demo_label_set_text_if_changed(s_wifi_status_label, "状态: 未初始化");
            demo_label_set_text_if_changed(s_wifi_link_label, "Link: --");
            demo_label_set_text_if_changed(s_wifi_ap_label, "WiFi: --");
            demo_label_set_text_if_changed(s_wifi_hint_label, "Hint: host NA");
            demo_label_set_text_if_changed(s_ble_status_label, "状态: 未初始化");
            demo_label_set_text_if_changed(s_ble_name_label, "Name: --");
            demo_label_set_text_if_changed(s_ble_link_label, "Link: --");
            demo_label_set_text_if_changed(s_ble_value_label, "GATT值: --");
            demo_label_set_text_if_changed(s_ble_hint_label, "BLE host NA");
        }
        demo_set_ble_adv_button(false, false);
        demo_obj_set_disabled(s_wifi_scan_btn, true);
        if (s_wifi_list != NULL) {
            demo_wifi_list_set_message(DEMO_WIFI_LIST_VIEW_C6_LINK_FAILED, "C6 Link Failed", status.wifi_scan_generation);
        }
        return;
    }

    demo_label_set_text_fmt_if_changed(s_wifi_status_label, "状态: %s",
                                       wireless_transport_state_to_text(status.transport_state));
    demo_label_set_text_fmt_if_changed(s_wifi_link_label, "C6:%s FW:%s RPC:%s",
                                       status.cp_target_name[0] != '\0' ? status.cp_target_name : "--",
                                       status.cp_fw_version[0] != '\0' ? status.cp_fw_version : "--",
                                       status.rpc_metadata_ready ? "OK" : "WARN");
    demo_label_set_text_fmt_if_changed(s_wifi_ap_label, "WiFi: %s AP=%" PRIu32,
                                       wireless_wifi_state_to_text(status.wifi_state),
                                       status.wifi_ap_count);
    char current_ssid_display[32];
    demo_label_set_text_fmt_if_changed(s_wifi_hint_label, "SSID=%s IP=%s",
                                       demo_wifi_ssid_display_text(status.current_ssid,
                                                                   current_ssid_display,
                                                                   sizeof(current_ssid_display)),
                                       status.current_ip[0] != '\0' ? status.current_ip : "--");
    demo_label_set_text_fmt_if_changed(s_ble_status_label, "状态: %s", wireless_ble_state_to_text(status.ble_state));
    demo_label_set_text_fmt_if_changed(s_ble_name_label, "Name: %s",
                                       status.ble_device_name[0] != '\0' ? status.ble_device_name : "--");
    demo_label_set_text_fmt_if_changed(s_ble_link_label, "Link: sync=%d conn=%d adv=%d",
                                       status.ble_host_synced,
                                       status.ble_connected,
                                       status.ble_advertising_enabled);
    demo_label_set_text_fmt_if_changed(s_ble_value_label, "GATT值: %u", (unsigned int)status.ble_test_value);
    demo_label_set_text_fmt_if_changed(s_ble_hint_label, "err=0x%x %s",
                                       (unsigned int)status.last_error,
                                       status.last_hint);

    bool wifi_actions_ready = status.hosted_stack_available &&
                              status.wifi_feature_enabled &&
                              status.transport_confirmed &&
                              status.transport_state == TEST_WIRELESS_TRANSPORT_READY;

    if (wifi_actions_ready) {
        demo_obj_set_disabled(s_wifi_scan_btn, false);
    } else {
        demo_obj_set_disabled(s_wifi_scan_btn, true);
    }

    bool ble_actions_ready = status.hosted_stack_available &&
                             status.ble_feature_enabled &&
                             status.transport_confirmed &&
                             status.transport_state == TEST_WIRELESS_TRANSPORT_READY &&
                             status.ble_state != TEST_WIRELESS_BLE_NOT_AVAILABLE &&
                             status.ble_state != TEST_WIRELESS_BLE_ERROR &&
                             (!status.ble_connected || status.ble_advertising_enabled);
    demo_set_ble_adv_button(ble_actions_ready, status.ble_advertising_enabled);

    demo_refresh_wifi_ap_list(&status);
#else
    demo_label_set_text_if_changed(s_wifi_status_label, "状态: 未开");
    demo_label_set_text_if_changed(s_wifi_link_label, "Link: --");
    demo_label_set_text_if_changed(s_wifi_ap_label, "WiFi: --");
    demo_label_set_text_if_changed(s_wifi_hint_label, "开 CONFIG_EXAMPLE_ENABLE_TEST_WIRELESS");
    demo_label_set_text_if_changed(s_ble_status_label, "状态: 未开");
    demo_label_set_text_if_changed(s_ble_name_label, "Name: --");
    demo_label_set_text_if_changed(s_ble_link_label, "Link: --");
    demo_label_set_text_if_changed(s_ble_value_label, "GATT值: --");
    demo_label_set_text_if_changed(s_ble_hint_label, "开 CONFIG_EXAMPLE_ENABLE_TEST_WIRELESS");
    demo_set_ble_adv_button(false, false);
    demo_obj_set_disabled(s_wifi_scan_btn, true);
    if (s_wifi_list != NULL) {
        demo_wifi_list_set_message(DEMO_WIFI_LIST_VIEW_DISABLED, "WiFi Disabled", 0);
    }
#endif
}

static const char *demo_font_diag_state_text(display_font_diag_state_t state)
{
    switch (state) {
    case DISPLAY_FONT_DIAG_OK:
        return "OK";
    case DISPLAY_FONT_DIAG_FALLBACK:
        return "Fallback";
    case DISPLAY_FONT_DIAG_ERROR:
        return "Error";
    case DISPLAY_FONT_DIAG_NOT_RUN:
    default:
        return "Not Run";
    }
}

static uint16_t demo_font_size_clamp(int32_t size_px)
{
    if (size_px < DISPLAY_FONT_SAMPLE_SIZE_MIN) {
        return DISPLAY_FONT_SAMPLE_SIZE_MIN;
    }
    if (size_px > DISPLAY_FONT_SAMPLE_SIZE_MAX) {
        return DISPLAY_FONT_SAMPLE_SIZE_MAX;
    }
    return (uint16_t)size_px;
}

static bool demo_font_apply_sample_size(uint16_t size_px)
{
    size_px = demo_font_size_clamp(size_px);
    if (s_font_sample_label == NULL) {
        return false;
    }

    const lv_font_t *font = display_font_get_freetype_sample_font(size_px);
    if (font == NULL) {
        return false;
    }

    s_font_sample_size_px = size_px;
    lv_obj_set_style_text_font(s_font_sample_label, font, 0);
    lv_obj_set_style_transform_scale(s_font_sample_label, 256, 0);
    lv_obj_center(s_font_sample_label);
    lv_obj_invalidate(s_font_sample_label);
    return true;
}

static bool demo_font_point_in_sample_area(const display_touch_point_t *point)
{
    if (point == NULL || s_font_sample_area == NULL) {
        return false;
    }

    lv_area_t area;
    lv_obj_get_coords(s_font_sample_area, &area);
    return point->x >= area.x1 && point->x <= area.x2 && point->y >= area.y1 && point->y <= area.y2;
}

static float demo_font_touch_distance(const display_touch_point_t *a, const display_touch_point_t *b)
{
    const float dx = (float)(a->x - b->x);
    const float dy = (float)(a->y - b->y);
    return sqrtf(dx * dx + dy * dy);
}

static void demo_update_font_touch_zoom(void)
{
    if (s_font_sample_area == NULL || s_font_sample_label == NULL) {
        return;
    }

    display_touch_snapshot_t snapshot = {0};
    if (display_touch_get_snapshot(&snapshot) != ESP_OK) {
        return;
    }

    display_touch_point_t area_points[2] = {0};
    uint8_t area_count = 0;
    for (uint8_t i = 0; i < snapshot.count && area_count < 2; i++) {
        if (demo_font_point_in_sample_area(&snapshot.points[i])) {
            area_points[area_count++] = snapshot.points[i];
        }
    }

    if (area_count < 2) {
        s_font_pinch_active = false;
        s_font_pinch_base_size_px = s_font_sample_size_px;
        s_font_pinch_base_distance = 0.0f;
        if (s_font_touch_label != NULL) {
            demo_label_set_text_fmt_if_changed(s_font_touch_label, "Touch: %u point(s) in area  Font: %upx",
                                               (unsigned int)area_count,
                                               (unsigned int)s_font_sample_size_px);
        }
        return;
    }

    const float distance = demo_font_touch_distance(&area_points[0], &area_points[1]);
    if (distance < 20.0f) {
        if (s_font_touch_label != NULL) {
            demo_label_set_text_if_changed(s_font_touch_label, "Touch: 2 points too close");
        }
        return;
    }

    if (!s_font_pinch_active) {
        s_font_pinch_active = true;
        s_font_pinch_base_size_px = s_font_sample_size_px;
        s_font_pinch_base_distance = distance;
    }

    if (!display_font_is_freetype_ready()) {
        if (s_font_touch_label != NULL) {
            demo_label_set_text_if_changed(s_font_touch_label, "Touch: 2 points  Font: static fallback");
        }
        return;
    }

    bool font_ok = true;
    if (s_font_pinch_base_distance > 0.01f) {
        const float scale = distance / s_font_pinch_base_distance;
        const uint16_t target_size = demo_font_size_clamp((int32_t)((float)s_font_pinch_base_size_px * scale));
        font_ok = demo_font_apply_sample_size(target_size);
    }

    if (s_font_touch_label != NULL) {
        demo_label_set_text_fmt_if_changed(s_font_touch_label, "Touch: 2 points  Font: %upx%s",
                                           (unsigned int)s_font_sample_size_px,
                                           font_ok ? "" : " create failed");
    }
}

static void demo_font_touch_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    demo_update_font_touch_zoom();
}

static void demo_update_font_card(void)
{
    if (s_font_status_label == NULL) {
        return;
    }

    const display_font_diagnostics_t *diag = display_font_get_diagnostics();
    if (diag == NULL) {
        demo_label_set_text_if_changed(s_font_status_label, "State: NA");
        demo_label_set_text_if_changed(s_font_file_label, "Font: static fallback");
        demo_label_set_text_if_changed(s_font_size_label, "Gesture: pinch with two fingers");
        demo_label_set_text_fmt_if_changed(s_font_touch_label, "Touch: 0 point(s) in area  Font: %upx",
                                           (unsigned int)s_font_sample_size_px);
        demo_label_set_text_if_changed(s_font_glyph_label, "FreeType: --");
        demo_label_set_text_if_changed(s_font_sample_label, "缩放测试 中文");
        return;
    }

    bool sample_font_ready = demo_font_apply_sample_size(s_font_sample_size_px);

    demo_label_set_text_fmt_if_changed(s_font_status_label, "State: %s stage=%s",
                                       demo_font_diag_state_text(diag->state),
                                       diag->stage[0] != '\0' ? diag->stage : "--");
    demo_label_set_text_fmt_if_changed(s_font_file_label, "Font: %s",
                                       sample_font_ready ? "FreeType dynamic" : "static fallback");
    demo_label_set_text_if_changed(s_font_size_label, "Gesture: pinch with two fingers");
    demo_label_set_text_fmt_if_changed(s_font_glyph_label, "Roles: ready=%" PRIu32 " fail=%" PRIu32 " %s",
                                       diag->role_ready_count,
                                       diag->role_fail_count,
                                       diag->first_error[0] != '\0' ? diag->first_error : "");
    demo_label_set_text_if_changed(s_font_sample_label, "缩放测试 中文");
}

static void demo_uvc_update_resolution_options(void)
{
    size_t count = TEST_UVC_FRAME_INFO_MAX;
    test_uvc_resolution_t latest[TEST_UVC_FRAME_INFO_MAX] = {0};
    esp_err_t ret = test_uvc_get_resolutions(latest, &count);
    if (ret != ESP_OK) {
        count = 0;
    }
    bool changed = count != s_uvc_resolution_count;
    if (!changed) {
        for (size_t i = 0; i < count; ++i) {
            if (latest[i].h_res != s_uvc_resolutions[i].h_res ||
                latest[i].v_res != s_uvc_resolutions[i].v_res ||
                latest[i].fps != s_uvc_resolutions[i].fps) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) {
        return;
    }

    memcpy(s_uvc_resolutions, latest, sizeof(s_uvc_resolutions));
    s_uvc_resolution_count = count;
    if (count == 0) {
        lv_dropdown_set_options(s_uvc_resolution_dropdown, "等待摄像头枚举");
        lv_obj_add_state(s_uvc_resolution_dropdown, LV_STATE_DISABLED);
        return;
    }

    char options[TEST_UVC_FRAME_INFO_MAX * 32] = {0};
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        int written = snprintf(options + used, sizeof(options) - used, "%s%ux%u @ %.0f FPS",
                               i == 0 ? "" : "\n", (unsigned)s_uvc_resolutions[i].h_res,
                               (unsigned)s_uvc_resolutions[i].v_res, s_uvc_resolutions[i].fps);
        if (written <= 0 || (size_t)written >= sizeof(options) - used) {
            break;
        }
        used += (size_t)written;
    }
    lv_dropdown_set_options(s_uvc_resolution_dropdown, options);
    lv_obj_clear_state(s_uvc_resolution_dropdown, LV_STATE_DISABLED);
}

static void demo_uvc_resolution_dropdown_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED || s_uvc_resolution_count == 0) {
        return;
    }
    uint32_t selected = lv_dropdown_get_selected(s_uvc_resolution_dropdown);
    if (selected >= s_uvc_resolution_count) {
        return;
    }
    const test_uvc_resolution_t *resolution = &s_uvc_resolutions[selected];
    esp_err_t ret = test_usb_role_restart_uvc(resolution->h_res, resolution->v_res);
    if (ret != ESP_OK) {
        demo_label_set_text_fmt_if_changed(s_uvc_status_label, "切换分辨率失败: %s", esp_err_to_name(ret));
        return;
    }
    lv_obj_add_state(s_uvc_resolution_dropdown, LV_STATE_DISABLED);
    demo_label_set_text_fmt_if_changed(s_uvc_status_label, "正在切换到 %ux%u...",
                                       (unsigned)resolution->h_res, (unsigned)resolution->v_res);
}

static void demo_uvc_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_uvc_screen == NULL) {
        return;
    }

    test_usb_role_status_t role_status = {0};
    if (test_usb_role_get_status(&role_status) != ESP_OK) {
        demo_label_set_text_if_changed(s_uvc_status_label, "UVC 角色未初始化");
        return;
    }

    if (role_status.switch_in_progress) {
        demo_label_set_text_if_changed(s_uvc_status_label, "UVC 初始化/切换中...");
    } else if (role_status.current_role != TEST_USB_ROLE_UVC_HOST) {
        demo_label_set_text_if_changed(s_uvc_status_label, "等待 UVC Host 启动");
    } else if (!role_status.uvc_device_connected) {
        demo_label_set_text_if_changed(s_uvc_status_label, "等待摄像头连接");
    } else if (!role_status.uvc_stream_started) {
        demo_label_set_text_if_changed(s_uvc_status_label, "摄像头已连接，正在打开视频流");
    } else {
        demo_label_set_text_fmt_if_changed(s_uvc_status_label, "实时预览: %ux%u @ %.1f FPS",
                                           (unsigned)role_status.uvc_negotiated_h_res,
                                           (unsigned)role_status.uvc_negotiated_v_res,
                                           role_status.uvc_negotiated_fps);
    }
    demo_label_set_text_fmt_if_changed(s_uvc_detail_label, "已解码 %" PRIu32 " 帧  错误 %" PRIu32,
                                       role_status.uvc_frames_decoded, role_status.uvc_decode_errors);

    demo_uvc_update_resolution_options();

    test_uvc_preview_info_t preview = {0};
    if (test_uvc_copy_preview(s_uvc_preview_image_buf, DEMO_UVC_PREVIEW_SIZE, &preview) == ESP_OK &&
        preview.frame_id != s_uvc_preview_frame_id) {
        s_uvc_preview_frame_id = preview.frame_id;
        esp_cache_msync(s_uvc_preview_image_buf, DEMO_UVC_PREVIEW_SIZE,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        lv_obj_invalidate(s_uvc_preview_image);
    }
}

static void demo_uvc_return_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_uvc_timer != NULL) {
        lv_timer_delete(s_uvc_timer);
        s_uvc_timer = NULL;
    }
    test_usb_role_switch(TEST_USB_ROLE_NONE);
    lv_screen_load(s_main_screen);
    lv_obj_delete(s_uvc_screen);
    s_uvc_screen = NULL;
    s_uvc_status_label = NULL;
    s_uvc_detail_label = NULL;
    s_uvc_resolution_dropdown = NULL;
    s_uvc_preview_image = NULL;
    heap_caps_free(s_uvc_preview_image_buf);
    s_uvc_preview_image_buf = NULL;
    memset(&s_uvc_preview_dsc, 0, sizeof(s_uvc_preview_dsc));
    s_uvc_preview_frame_id = 0;
    s_uvc_resolution_count = 0;
}

static void demo_uvc_enter_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_uvc_screen != NULL) {
        lv_screen_load(s_uvc_screen);
        return;
    }

    s_uvc_preview_image_buf = heap_caps_calloc(1, DEMO_UVC_PREVIEW_SIZE,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_uvc_preview_image_buf == NULL) {
        ESP_LOGE(TAG, "allocate UVC UI preview buffer failed");
        return;
    }

    s_uvc_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_uvc_screen, lv_color_hex(0x101820), 0);
    lv_obj_set_style_pad_all(s_uvc_screen, 12, 0);
    lv_obj_clear_flag(s_uvc_screen, LV_OBJ_FLAG_SCROLLABLE);
    demo_set_text_role(s_uvc_screen, DISPLAY_FONT_ROLE_BODY);

    lv_obj_t *title = lv_label_create(s_uvc_screen);
    lv_label_set_text(title, "UVC 摄像头测试");
    demo_config_label(title, DISPLAY_FONT_ROLE_TITLE);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    lv_obj_t *return_btn = lv_button_create(s_uvc_screen);
    lv_obj_set_size(return_btn, 112, 42);
    lv_obj_align(return_btn, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_event_cb(return_btn, demo_uvc_return_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *return_label = lv_label_create(return_btn);
    lv_label_set_text(return_label, "返回主页");
    demo_set_text_role(return_label, DISPLAY_FONT_ROLE_BODY);
    lv_obj_center(return_label);

    s_uvc_status_label = lv_label_create(s_uvc_screen);
    s_uvc_detail_label = lv_label_create(s_uvc_screen);
    demo_config_label(s_uvc_status_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_uvc_detail_label, DISPLAY_FONT_ROLE_SMALL);
    lv_obj_set_style_text_color(s_uvc_status_label, lv_color_hex(0xE8F1F5), 0);
    lv_obj_set_style_text_color(s_uvc_detail_label, lv_color_hex(0xA9C0C9), 0);
    lv_obj_align(s_uvc_status_label, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_align(s_uvc_detail_label, LV_ALIGN_TOP_MID, 0, 66);

    s_uvc_resolution_dropdown = lv_dropdown_create(s_uvc_screen);
    lv_obj_set_size(s_uvc_resolution_dropdown, 270, 42);
    lv_obj_align(s_uvc_resolution_dropdown, LV_ALIGN_TOP_MID, 0, 92);
    demo_set_text_role(s_uvc_resolution_dropdown, DISPLAY_FONT_ROLE_BODY);
    lv_obj_t *uvc_resolution_list = lv_dropdown_get_list(s_uvc_resolution_dropdown);
    demo_set_text_role(uvc_resolution_list, DISPLAY_FONT_ROLE_BODY);
    lv_obj_set_style_text_font(uvc_resolution_list,
                               demo_get_role_font(DISPLAY_FONT_ROLE_BODY),
                               LV_PART_SELECTED);
    lv_dropdown_set_options(s_uvc_resolution_dropdown, "等待摄像头枚举");
    lv_obj_add_state(s_uvc_resolution_dropdown, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_uvc_resolution_dropdown, demo_uvc_resolution_dropdown_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    s_uvc_preview_dsc = (lv_image_dsc_t) {
        .header = {
            .cf = LV_COLOR_FORMAT_RGB565,
            .w = TEST_UVC_PREVIEW_H_RES,
            .h = TEST_UVC_PREVIEW_V_RES,
            .stride = TEST_UVC_PREVIEW_H_RES * 2U,
        },
        .data_size = DEMO_UVC_PREVIEW_SIZE,
        .data = s_uvc_preview_image_buf,
    };
    s_uvc_preview_image = lv_image_create(s_uvc_screen);
    lv_image_set_src(s_uvc_preview_image, &s_uvc_preview_dsc);
    lv_obj_set_size(s_uvc_preview_image, TEST_UVC_PREVIEW_H_RES, TEST_UVC_PREVIEW_V_RES);
    lv_obj_align(s_uvc_preview_image, LV_ALIGN_BOTTOM_MID, 0, 0);

    s_uvc_preview_frame_id = 0;
    s_uvc_resolution_count = 0;
    s_uvc_timer = lv_timer_create(demo_uvc_timer_cb, TEST_UVC_PREVIEW_REFRESH_MS, NULL);
    lv_screen_load(s_uvc_screen);
    esp_err_t ret = test_usb_role_switch(TEST_USB_ROLE_UVC_HOST);
    if (ret != ESP_OK) {
        demo_label_set_text_fmt_if_changed(s_uvc_status_label, "启动 UVC 失败: %s", esp_err_to_name(ret));
    }
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
    demo_update_font_card();
    demo_ui_report_updates_if_ready();
}

void example_lvgl_demo_ui(lv_display_t *disp, demo_ui_progress_cb_t progress_cb)
{
    lv_theme_default_init(disp,
                          lv_color_hex(0x1F5C8A),
                          lv_color_hex(0xD97A3A),
                          false,
                          demo_get_role_font(DISPLAY_FONT_ROLE_BODY));

    lv_obj_t *screen = lv_display_get_screen_active(disp);
    s_main_screen = screen;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xE6D8C2), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0xF6EFE4), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_pad_all(screen, 14, 0);
    lv_obj_set_style_pad_row(screen, 12, 0);
    lv_obj_set_layout(screen, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(screen, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_AUTO);
    demo_set_text_role(screen, DISPLAY_FONT_ROLE_BODY);

    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(header, 12, 0);
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
    demo_config_label(title, DISPLAY_FONT_ROLE_TITLE);

    lv_obj_t *subtitle = lv_label_create(header);
    lv_label_set_text(subtitle, "显示/触摸 + RTC + USB/UVC + 音频 + BAT + WiFi + BLE");
    demo_config_label(subtitle, DISPLAY_FONT_ROLE_SMALL);
    demo_build_progress(progress_cb, "header");

    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_set_style_pad_row(row, 12, 0);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *touch_card = demo_create_card(row, "1. 显示与触摸", 488, 312);
    s_touch_status_label = lv_label_create(touch_card);
    s_touch_count_label = lv_label_create(touch_card);
    s_touch_point_label = lv_label_create(touch_card);
    s_touch_hint_label = lv_label_create(touch_card);
    demo_config_label(s_touch_status_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_touch_count_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_touch_point_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_touch_hint_label, DISPLAY_FONT_ROLE_BODY);

    lv_obj_t *touch_pad = lv_obj_create(touch_card);
    lv_obj_set_size(touch_pad, lv_pct(100), 116);
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
    demo_set_text_role(touch_pad_label, DISPLAY_FONT_ROLE_BODY);
    lv_obj_center(touch_pad_label);
    demo_build_progress(progress_cb, "touch");

    lv_obj_t *rtc_card = demo_create_card(row, "2. RTC", 488, 220);
    s_rtc_status_label = lv_label_create(rtc_card);
    s_rtc_time_label = lv_label_create(rtc_card);
    s_rtc_sync_label = lv_label_create(rtc_card);
    demo_config_label(s_rtc_status_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_rtc_time_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_rtc_sync_label, DISPLAY_FONT_ROLE_BODY);
    demo_build_progress(progress_cb, "rtc");

    lv_obj_t *usb_card = demo_create_card(row, "3. USB Device", 488, 238);
    s_usb_status_label = lv_label_create(usb_card);
    s_usb_line_label = lv_label_create(usb_card);
    s_usb_stats_label = lv_label_create(usb_card);
    s_usb_hint_label = lv_label_create(usb_card);
    demo_config_label(s_usb_status_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_usb_line_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_usb_stats_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_usb_hint_label, DISPLAY_FONT_ROLE_SMALL);
    lv_obj_set_style_text_color(s_usb_hint_label, lv_color_hex(0x5E4C38), 0);

    lv_obj_t *usb_btn_row = lv_obj_create(usb_card);
    lv_obj_set_width(usb_btn_row, lv_pct(100));
    lv_obj_set_height(usb_btn_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(usb_btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usb_btn_row, 0, 0);
    lv_obj_set_style_pad_all(usb_btn_row, 0, 0);
    lv_obj_set_style_pad_column(usb_btn_row, 10, 0);
    lv_obj_set_layout(usb_btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(usb_btn_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_clear_flag(usb_btn_row, LV_OBJ_FLAG_SCROLLABLE);

    s_usb_switch_btn = lv_button_create(usb_btn_row);
    lv_obj_set_size(s_usb_switch_btn, 160, 48);
    lv_obj_add_event_cb(s_usb_switch_btn, usb_switch_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *usb_switch_label = lv_label_create(s_usb_switch_btn);
    lv_label_set_text(usb_switch_label, "切换到 UVC");
    demo_set_text_role(usb_switch_label, DISPLAY_FONT_ROLE_BODY);
    lv_obj_center(usb_switch_label);

    s_uvc_enter_btn = lv_button_create(usb_btn_row);
    lv_obj_set_size(s_uvc_enter_btn, 180, 48);
    lv_obj_add_event_cb(s_uvc_enter_btn, demo_uvc_enter_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *uvc_enter_label = lv_label_create(s_uvc_enter_btn);
    lv_label_set_text(uvc_enter_label, "进入 UVC 测试");
    demo_set_text_role(uvc_enter_label, DISPLAY_FONT_ROLE_BODY);
    lv_obj_center(uvc_enter_label);

    demo_build_progress(progress_cb, "usb");

    lv_obj_t *audio_card = demo_create_card(row, "4. 音频", 488, 238);
    s_audio_status_label = lv_label_create(audio_card);
    s_audio_size_label = lv_label_create(audio_card);
    s_audio_peak_label = lv_label_create(audio_card);
    s_audio_input_label = lv_label_create(audio_card);
    demo_config_label(s_audio_status_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_audio_size_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_audio_peak_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_audio_input_label, DISPLAY_FONT_ROLE_BODY);

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
    lv_obj_set_size(s_audio_music_btn, 135, 48);
    lv_obj_add_event_cb(s_audio_music_btn, audio_music_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *music_label = lv_label_create(s_audio_music_btn);
    lv_label_set_text(music_label, "播放内置音乐");
    demo_set_text_role(music_label, DISPLAY_FONT_ROLE_BODY);
    lv_obj_center(music_label);

    s_audio_record_btn = lv_button_create(audio_btn_row);
    lv_obj_set_size(s_audio_record_btn, 110, 48);
    lv_obj_add_event_cb(s_audio_record_btn, audio_record_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *record_label = lv_label_create(s_audio_record_btn);
    lv_label_set_text(record_label, "开始录音");
    demo_set_text_role(record_label, DISPLAY_FONT_ROLE_BODY);
    lv_obj_center(record_label);

    s_audio_play_btn = lv_button_create(audio_btn_row);
    lv_obj_set_size(s_audio_play_btn, 110, 48);
    lv_obj_add_event_cb(s_audio_play_btn, audio_play_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *play_label = lv_label_create(s_audio_play_btn);
    lv_label_set_text(play_label, "播放录音");
    demo_set_text_role(play_label, DISPLAY_FONT_ROLE_BODY);
    lv_obj_center(play_label);
    demo_build_progress(progress_cb, "audio");

    lv_obj_t *battery_card = demo_create_card(row, "5. BAT", 488, 238);
    s_battery_status_label = lv_label_create(battery_card);
    s_battery_adc_label = lv_label_create(battery_card);
    s_battery_voltage_label = lv_label_create(battery_card);
    s_battery_percent_label = lv_label_create(battery_card);
    s_battery_hint_label = lv_label_create(battery_card);
    demo_config_label(s_battery_status_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_battery_adc_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_battery_voltage_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_battery_percent_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_battery_hint_label, DISPLAY_FONT_ROLE_SMALL);
    lv_obj_set_style_text_color(s_battery_hint_label, lv_color_hex(0x5E4C38), 0);
    demo_build_progress(progress_cb, "battery");

    lv_obj_t *wifi_card = demo_create_card(row, "6. WiFi", 488, 388);
    s_wifi_status_label = lv_label_create(wifi_card);
    s_wifi_link_label = lv_label_create(wifi_card);
    s_wifi_ap_label = lv_label_create(wifi_card);
    s_wifi_hint_label = lv_label_create(wifi_card);
    demo_config_label(s_wifi_status_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_wifi_link_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_wifi_ap_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_wifi_hint_label, DISPLAY_FONT_ROLE_SMALL);
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
    lv_obj_set_size(s_wifi_scan_btn, 124, 48);
    lv_obj_add_event_cb(s_wifi_scan_btn, wireless_wifi_scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wifi_scan_label = lv_label_create(s_wifi_scan_btn);
    lv_label_set_text(wifi_scan_label, "WiFi Scan");
    demo_set_text_role(wifi_scan_label, DISPLAY_FONT_ROLE_BODY);
    lv_obj_center(wifi_scan_label);

    s_wifi_list = lv_obj_create(wifi_card);
    lv_obj_set_size(s_wifi_list, lv_pct(100), 170);
    lv_obj_set_style_radius(s_wifi_list, 0, 0);
    lv_obj_set_style_border_width(s_wifi_list, 1, 0);
    lv_obj_set_style_border_color(s_wifi_list, lv_color_hex(0xD7C7AF), 0);
    lv_obj_set_style_bg_color(s_wifi_list, lv_color_hex(0xFFF9EF), 0);
    lv_obj_set_style_pad_all(s_wifi_list, 6, 0);
    lv_obj_set_style_pad_row(s_wifi_list, 6, 0);
    lv_obj_set_layout(s_wifi_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_wifi_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_wifi_list, LV_SCROLLBAR_MODE_AUTO);
    demo_wifi_list_add_text("Tap Scan");
    demo_build_progress(progress_cb, "wifi");

    lv_obj_t *ble_card = demo_create_card(row, "7. BLE", 488, 282);
    s_ble_status_label = lv_label_create(ble_card);
    s_ble_name_label = lv_label_create(ble_card);
    s_ble_link_label = lv_label_create(ble_card);
    s_ble_value_label = lv_label_create(ble_card);
    s_ble_hint_label = lv_label_create(ble_card);
    demo_config_label(s_ble_status_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_ble_name_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_ble_link_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_ble_value_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_ble_hint_label, DISPLAY_FONT_ROLE_SMALL);
    lv_obj_set_style_text_color(s_ble_hint_label, lv_color_hex(0x5E4C38), 0);
    lv_label_set_text(s_ble_status_label, "状态: host NA");
    lv_label_set_text(s_ble_name_label, "Name: --");
    lv_label_set_text(s_ble_link_label, "Link: --");
    lv_label_set_text(s_ble_value_label, "GATT值: --");
    lv_label_set_text(s_ble_hint_label, "BLE Not Ready");

    s_ble_adv_btn = lv_button_create(ble_card);
    lv_obj_set_size(s_ble_adv_btn, 124, 48);
    lv_obj_add_event_cb(s_ble_adv_btn, wireless_ble_adv_btn_cb, LV_EVENT_CLICKED, NULL);
    s_ble_adv_btn_label = lv_label_create(s_ble_adv_btn);
    demo_set_text_role(s_ble_adv_btn_label, DISPLAY_FONT_ROLE_BODY);
    lv_label_set_text(s_ble_adv_btn_label, "Adv On");
    lv_obj_center(s_ble_adv_btn_label);
    lv_obj_add_state(s_ble_adv_btn, LV_STATE_DISABLED);
    demo_build_progress(progress_cb, "ble");

    lv_obj_t *font_card = demo_create_card(row, "8. FreeType", 488, 420);
    s_font_status_label = lv_label_create(font_card);
    s_font_file_label = lv_label_create(font_card);
    s_font_size_label = lv_label_create(font_card);
    s_font_touch_label = lv_label_create(font_card);
    s_font_glyph_label = lv_label_create(font_card);
    demo_config_label(s_font_status_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_font_file_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_font_size_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_font_touch_label, DISPLAY_FONT_ROLE_BODY);
    demo_config_label(s_font_glyph_label, DISPLAY_FONT_ROLE_BODY);

    s_font_sample_area = lv_obj_create(font_card);
    lv_obj_set_size(s_font_sample_area, lv_pct(100), 250);
    lv_obj_set_style_radius(s_font_sample_area, 8, 0);
    lv_obj_set_style_bg_color(s_font_sample_area, lv_color_hex(0xFFF9EF), 0);
    lv_obj_set_style_border_width(s_font_sample_area, 1, 0);
    lv_obj_set_style_border_color(s_font_sample_area, lv_color_hex(0xD7C7AF), 0);
    lv_obj_clear_flag(s_font_sample_area, LV_OBJ_FLAG_SCROLLABLE);

    s_font_sample_label = lv_label_create(s_font_sample_area);
    demo_config_label(s_font_sample_label, DISPLAY_FONT_ROLE_TITLE);
    lv_label_set_text(s_font_sample_label, "缩放测试 中文");
    lv_obj_set_style_text_color(s_font_sample_label, lv_color_hex(0x17324D), 0);
    lv_obj_set_style_transform_scale(s_font_sample_label, 256, 0);
    lv_obj_center(s_font_sample_label);

    demo_build_progress(progress_cb, "font");

    lv_obj_t *top_layer = lv_display_get_layer_top(disp);
    s_wifi_dialog_mask = lv_obj_create(top_layer);
    lv_obj_remove_style_all(s_wifi_dialog_mask);
    lv_obj_set_size(s_wifi_dialog_mask, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_wifi_dialog_mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_wifi_dialog_mask, LV_OPA_50, 0);
    lv_obj_add_flag(s_wifi_dialog_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wifi_dialog_mask, LV_OBJ_FLAG_CLICKABLE);

    s_wifi_dialog = lv_obj_create(s_wifi_dialog_mask);
    lv_obj_set_size(s_wifi_dialog, 520, 270);
    lv_obj_center(s_wifi_dialog);
    lv_obj_set_style_radius(s_wifi_dialog, 12, 0);
    lv_obj_set_style_pad_all(s_wifi_dialog, 16, 0);
    lv_obj_set_style_pad_row(s_wifi_dialog, 10, 0);
    lv_obj_set_layout(s_wifi_dialog, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_wifi_dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_wifi_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dialog_title = lv_label_create(s_wifi_dialog);
    lv_label_set_text(dialog_title, "WiFi Join");
    demo_config_label(dialog_title, DISPLAY_FONT_ROLE_CARD_TITLE);

    s_wifi_dialog_ssid_label = lv_label_create(s_wifi_dialog);
    demo_config_label(s_wifi_dialog_ssid_label, DISPLAY_FONT_ROLE_BODY);
    lv_label_set_text(s_wifi_dialog_ssid_label, "SSID: --");

    s_wifi_password_ta = lv_textarea_create(s_wifi_dialog);
    lv_obj_set_width(s_wifi_password_ta, lv_pct(100));
    demo_set_text_role(s_wifi_password_ta, DISPLAY_FONT_ROLE_BODY);
    lv_textarea_set_one_line(s_wifi_password_ta, true);
    lv_textarea_set_password_mode(s_wifi_password_ta, true);
    lv_textarea_set_password_bullet(s_wifi_password_ta, "*");
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
    lv_obj_set_size(connect_btn, 110, 48);
    lv_obj_add_event_cb(connect_btn, wifi_dialog_connect_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *connect_label = lv_label_create(connect_btn);
    demo_set_text_role(connect_label, DISPLAY_FONT_ROLE_BODY);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_center(connect_label);

    lv_obj_t *cancel_btn = lv_button_create(dialog_btn_row);
    lv_obj_set_size(cancel_btn, 110, 48);
    lv_obj_add_event_cb(cancel_btn, wifi_dialog_cancel_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    demo_set_text_role(cancel_label, DISPLAY_FONT_ROLE_BODY);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);
    demo_build_progress(progress_cb, "dialog");

    s_wifi_keyboard = lv_keyboard_create(top_layer);
    lv_obj_set_size(s_wifi_keyboard, lv_pct(100), 220);
    lv_obj_set_style_text_font(s_wifi_keyboard, demo_get_role_font(DISPLAY_FONT_ROLE_BODY), LV_PART_ITEMS);
    lv_obj_align(s_wifi_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_map(s_wifi_keyboard,
                        LV_KEYBOARD_MODE_TEXT_LOWER,
                        s_wifi_keyboard_lower_map,
                        s_wifi_keyboard_lower_ctrl_map);
    lv_keyboard_set_map(s_wifi_keyboard,
                        LV_KEYBOARD_MODE_TEXT_UPPER,
                        s_wifi_keyboard_upper_map,
                        s_wifi_keyboard_upper_ctrl_map);
    lv_keyboard_set_map(s_wifi_keyboard,
                        LV_KEYBOARD_MODE_SPECIAL,
                        s_wifi_keyboard_special_map,
                        s_wifi_keyboard_special_ctrl_map);
    lv_keyboard_set_mode(s_wifi_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_wifi_keyboard, NULL);
    lv_obj_add_event_cb(s_wifi_keyboard,
                        wifi_keyboard_preprocess_cb,
                        LV_EVENT_VALUE_CHANGED | LV_EVENT_PREPROCESS,
                        NULL);
    lv_obj_add_event_cb(s_wifi_keyboard, wifi_keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    demo_build_progress(progress_cb, "keyboard");

    demo_update_touch_card();
    demo_update_rtc_card();
    demo_update_usb_card();
    demo_update_audio_card();
    demo_update_battery_card();
    demo_update_wireless_card();
    demo_update_font_card();
    demo_build_progress(progress_cb, "initial-update");
    lv_timer_create(demo_font_touch_timer_cb, 40, NULL);
    lv_timer_create(demo_ui_timer_cb, 250, NULL);
}
