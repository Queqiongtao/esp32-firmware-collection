#include "clock_settings_menu.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static constexpr uint32_t SLEEP_OPTIONS_SEC[] = {
    60,
    5 * 60,
    15 * 60,
    0,
};
static constexpr const char *SLEEP_OPTION_LABELS[] = {
    "1m",
    "5m",
    "15m",
    "Off",
};
static constexpr size_t SLEEP_OPTION_COUNT = sizeof(SLEEP_OPTIONS_SEC) / sizeof(SLEEP_OPTIONS_SEC[0]);
static constexpr uint32_t MENU_PAGE_COUNT = 5;
static constexpr int32_t MENU_PAGE_HEIGHT = 250;
static constexpr int32_t MENU_PAGE_GAP = 14;
static constexpr int32_t MENU_PAGE_STRIDE = MENU_PAGE_HEIGHT + MENU_PAGE_GAP;

static const char *const MENU_TAB_SYMBOLS[MENU_PAGE_COUNT] = {
    LV_SYMBOL_EYE_OPEN,
    LV_SYMBOL_POWER,
    LV_SYMBOL_REFRESH,
    LV_SYMBOL_WIFI,
    LV_SYMBOL_AUDIO,
};
static const char *const MENU_TAB_LABELS[MENU_PAGE_COUNT] = {
    "Display",
    "Power",
    "Motion",
    "WiFi",
    "Sound",
};
static constexpr int32_t MENU_TAB_X[MENU_PAGE_COUNT] = {0, 60, 120, 180, 240};
static constexpr int32_t MENU_TAB_Y[MENU_PAGE_COUNT] = {0, 6, 10, 6, 0};

static constexpr uint8_t WIFI_KB_LAYER_LOWER = 0;
static constexpr uint8_t WIFI_KB_LAYER_UPPER = 1;
static constexpr uint8_t WIFI_KB_LAYER_SYMBOL = 2;

static const char *const WIFI_KB_LOWER_MAP[] = {
    "a", "b", "c", "d", "e", "f", "\n",
    "g", "h", "i", "j", "k", "l", "\n",
    "m", "n", "o", "p", "q", "r", "\n",
    "s", "t", "u", "v", "w", "x", "\n",
    "y", "z", "Aa", "123", "Space", LV_SYMBOL_BACKSPACE, ""
};
static const char *const WIFI_KB_UPPER_MAP[] = {
    "A", "B", "C", "D", "E", "F", "\n",
    "G", "H", "I", "J", "K", "L", "\n",
    "M", "N", "O", "P", "Q", "R", "\n",
    "S", "T", "U", "V", "W", "X", "\n",
    "Y", "Z", "Aa", "123", "Space", LV_SYMBOL_BACKSPACE, ""
};
static const char *const WIFI_KB_SYMBOL_MAP[] = {
    "1", "2", "3", "4", "5", "6", "\n",
    "7", "8", "9", "0", "-", "_", "\n",
    ".", ",", ":", ";", "!", "?", "\n",
    "@", "#", "$", "%", "^", "&", "\n",
    "*", "(", ")", "+", "=", "/", "\n",
    "abc", "Space", LV_SYMBOL_BACKSPACE, "'", "\"", "\\", ""
};

static constexpr uint32_t MENU_COLOR_BG = 0x000000;
static constexpr uint32_t MENU_COLOR_SURFACE = 0x241B3A;
static constexpr uint32_t MENU_COLOR_SURFACE_PRESSED = 0x2E2348;
static constexpr uint32_t MENU_COLOR_ACCENT = 0x5EEAD4;
static constexpr uint32_t MENU_COLOR_WARM = 0xFDBA74;
static constexpr uint32_t MENU_COLOR_TAB = 0xFF6F91;
static constexpr uint32_t MENU_COLOR_TEXT = 0xF5F3FF;
static constexpr uint32_t MENU_COLOR_MUTED = 0x8B86A3;
static constexpr uint32_t MENU_COLOR_BORDER = 0x3A3050;
static constexpr uint32_t MENU_COLOR_TRACK = 0x241B3A;
static constexpr uint32_t MENU_COLOR_ACCENT_TEXT = 0x001410;
static constexpr uint32_t MENU_COLOR_WARM_TEXT = 0x1A0F00;
static constexpr uint32_t MENU_COLOR_TAB_TEXT = 0x1A0710;

static lv_obj_t *s_menu_root = nullptr;
static lv_obj_t *s_menu_content = nullptr;
static lv_obj_t *s_menu_wheel = nullptr;
static lv_obj_t *s_brightness_value = nullptr;
static lv_obj_t *s_auto_rotate_status = nullptr;
static lv_obj_t *s_power_card = nullptr;
static lv_obj_t *s_power_battery = nullptr;
static lv_obj_t *s_power_battery_detail = nullptr;
static lv_obj_t *s_power_detail_overlay = nullptr;
static lv_obj_t *s_power_detail_percent = nullptr;
static lv_obj_t *s_power_detail_voltage = nullptr;
static lv_obj_t *s_power_detail_state = nullptr;
static lv_obj_t *s_power_detail_extra = nullptr;
static lv_obj_t *s_power_detail_config = nullptr;
static clock_settings_menu_power_status_t s_power_status = {};
static bool s_power_status_valid = false;
static lv_obj_t *s_wifi_status = nullptr;
static lv_obj_t *s_wifi_switch = nullptr;
static lv_obj_t *s_ntp_switch = nullptr;
static lv_obj_t *s_ntp_sync_button = nullptr;
static lv_obj_t *s_wifi_detail = nullptr;
static lv_obj_t *s_wifi_setup_overlay = nullptr;
static lv_obj_t *s_wifi_summary_view = nullptr;
static lv_obj_t *s_wifi_edit_view = nullptr;
static lv_obj_t *s_wifi_scan_view = nullptr;
static lv_obj_t *s_wifi_scan_list = nullptr;
static lv_obj_t *s_wifi_rescan_button = nullptr;
static lv_timer_t *s_wifi_scan_timer = nullptr;
static bool s_wifi_scan_waiting = false;
static lv_obj_t *s_faces_overlay = nullptr;
static uint32_t s_faces_page = 0;
static uint32_t s_faces_candidate = 0;
static bool s_faces_submitted = false;
static lv_obj_t *s_faces_cards[4] = {};
static lv_obj_t *s_faces_apply = nullptr;
static lv_obj_t *s_wifi_summary_ssid = nullptr;
static lv_obj_t *s_wifi_summary_password = nullptr;
static lv_obj_t *s_wifi_edit_title = nullptr;
static lv_obj_t *s_wifi_input = nullptr;
static lv_obj_t *s_wifi_password_eye = nullptr;
static lv_obj_t *s_wifi_password_eye_label = nullptr;
static lv_obj_t *s_wifi_keyboard = nullptr;
static lv_obj_t *s_wifi_key_popover = nullptr;
static lv_obj_t *s_wifi_key_popover_label = nullptr;
static lv_obj_t *s_wifi_setup_status = nullptr;
static lv_obj_t *s_sound_body = nullptr;
static lv_obj_t *s_sound_master_switch = nullptr;
static lv_obj_t *s_sound_volume_slider = nullptr;
static lv_obj_t *s_sound_volume_value = nullptr;
static lv_obj_t *s_ui_sounds_switch = nullptr;
static lv_obj_t *s_chime_switch = nullptr;
static lv_obj_t *s_alarm_list_overlay = nullptr;
static lv_obj_t *s_alarm_edit_overlay = nullptr;
static lv_obj_t *s_alarm_list_values[CLOCK_ALARM_COUNT] = {};
static lv_obj_t *s_alarm_list_details[CLOCK_ALARM_COUNT] = {};
static lv_obj_t *s_alarm_edit_time = nullptr;
static lv_obj_t *s_alarm_edit_weekdays[7] = {};
static lv_obj_t *s_alarm_edit_sound = nullptr;
static lv_obj_t *s_alarm_edit_enable = nullptr;
static lv_obj_t *s_timer_overlay = nullptr;
static lv_obj_t *s_timer_presets[4] = {};
static lv_obj_t *s_timer_remaining = nullptr;
static lv_obj_t *s_timer_status = nullptr;
static lv_obj_t *s_timer_start_button = nullptr;
static lv_obj_t *s_timer_pause_button = nullptr;
static lv_obj_t *s_sleep_overlay = nullptr;
static lv_obj_t *s_audio_test_overlay = nullptr;
static lv_obj_t *s_sleep_type_buttons[2] = {};
static lv_obj_t *s_sleep_duration_buttons[3] = {};
static lv_obj_t *s_sleep_status = nullptr;
static lv_obj_t *s_sleep_start_button = nullptr;
static uint8_t s_alarm_edit_index = 0;
static clock_alarm_t s_alarm_edit = {};
static uint32_t s_timer_selected_minutes = 25;
static bool s_timer_pomodoro_selected = false;
static bool s_timer_status_running = false;
static bool s_timer_status_paused = false;
static bool s_timer_status_pomodoro = false;
static bool s_timer_status_break = false;
static uint32_t s_timer_status_remaining_sec = 0;
static bool s_sleep_brown_selected = false;
static uint32_t s_sleep_minutes_selected = 15;
static bool s_sleep_status_playing = false;
static bool s_sleep_status_usb = false;
static uint8_t s_wifi_keyboard_layer = 0;
static uint32_t s_suppress_scroll_sound_until_ms = 0;
static bool s_updating_wifi_ui = false;
static bool s_wifi_has_credentials = false;
static bool s_wifi_connected = false;
static bool s_wifi_password_visible = false;
static bool s_wifi_password_touched = false;
static bool s_wifi_password_dirty = false;
static bool s_wifi_input_updating = false;
static char s_wifi_ssid_buffer[33] = {};
static char s_wifi_password_buffer[65] = {};
static char s_wifi_saved_ssid[33] = {};
static char s_wifi_scan_ssids[8][33] = {};
static uint8_t s_wifi_edit_field = 0;
static lv_obj_t *s_page_tabs[MENU_PAGE_COUNT] = {};
static lv_obj_t *s_page_tab_icons[MENU_PAGE_COUNT] = {};
static lv_obj_t *s_page_tab_labels[MENU_PAGE_COUNT] = {};
static lv_obj_t *s_sleep_buttons[SLEEP_OPTION_COUNT] = {};
static lv_obj_t *s_sleep_labels[SLEEP_OPTION_COUNT] = {};
static clock_settings_menu_config_t s_config = {};
static bool s_menu_closing = false;
static clock_settings_menu_close_done_cb_t s_close_done_cb = nullptr;
static void *s_close_done_ctx = nullptr;

static void set_wheel_scroll_enabled(bool enabled);
static void open_wifi_setup(void);
static void on_power_card_clicked(lv_event_t *event);
static void open_alarm_list(void);
static void open_alarm_editor(uint8_t index);
static void open_timer_overlay(void);
static void open_sleep_overlay(void);
static void update_timer_ui(void);
static void update_sleep_ui(void);
static void update_alarm_list(void);
static void play_ui_sound(clock_settings_menu_ui_sound_t sound);

static void reset_wifi_setup_pointers(void)
{
    if (s_wifi_scan_timer != nullptr) {
        lv_timer_delete(s_wifi_scan_timer);
        s_wifi_scan_timer = nullptr;
    }
    s_wifi_scan_waiting = false;
    s_wifi_setup_overlay = nullptr;
    s_wifi_summary_view = nullptr;
    s_wifi_edit_view = nullptr;
    s_wifi_scan_view = nullptr;
    s_wifi_scan_list = nullptr;
    s_wifi_rescan_button = nullptr;
    s_wifi_summary_ssid = nullptr;
    s_wifi_summary_password = nullptr;
    s_wifi_edit_title = nullptr;
    s_wifi_input = nullptr;
    s_wifi_password_eye = nullptr;
    s_wifi_password_eye_label = nullptr;
    s_wifi_keyboard = nullptr;
    s_wifi_key_popover = nullptr;
    s_wifi_key_popover_label = nullptr;
    s_wifi_setup_status = nullptr;
    s_updating_wifi_ui = false;
    s_wifi_password_visible = false;
    s_wifi_password_touched = false;
    s_wifi_password_dirty = false;
    s_wifi_input_updating = false;
    s_wifi_edit_field = 0;
    s_wifi_keyboard_layer = 0;
}

static void reset_sound_pointers(void)
{
    s_sound_body = nullptr;
    s_sound_master_switch = nullptr;
    s_sound_volume_slider = nullptr;
    s_sound_volume_value = nullptr;
    s_ui_sounds_switch = nullptr;
    s_chime_switch = nullptr;
    s_alarm_list_overlay = nullptr;
    s_alarm_edit_overlay = nullptr;
    for (size_t i = 0; i < CLOCK_ALARM_COUNT; ++i) {
        s_alarm_list_values[i] = nullptr;
        s_alarm_list_details[i] = nullptr;
    }
    s_alarm_edit_time = nullptr;
    for (size_t i = 0; i < 7; ++i) {
        s_alarm_edit_weekdays[i] = nullptr;
    }
    s_alarm_edit_sound = nullptr;
    s_alarm_edit_enable = nullptr;
    s_timer_overlay = nullptr;
    for (size_t i = 0; i < 4; ++i) {
        s_timer_presets[i] = nullptr;
    }
    s_timer_remaining = nullptr;
    s_timer_status = nullptr;
    s_timer_start_button = nullptr;
    s_timer_pause_button = nullptr;
    s_sleep_overlay = nullptr;
    s_audio_test_overlay = nullptr;
    for (size_t i = 0; i < 2; ++i) {
        s_sleep_type_buttons[i] = nullptr;
    }
    for (size_t i = 0; i < 3; ++i) {
        s_sleep_duration_buttons[i] = nullptr;
    }
    s_sleep_status = nullptr;
    s_sleep_start_button = nullptr;
}

static void reset_menu_widget_pointers(void)
{
    reset_sound_pointers();
    s_faces_overlay = nullptr;
    s_faces_apply = nullptr;
    for (auto &card : s_faces_cards) {
        card = nullptr;
    }
    s_faces_candidate = 0;
    s_faces_submitted = false;
    s_suppress_scroll_sound_until_ms = 0;
    s_menu_content = nullptr;
    s_menu_wheel = nullptr;
    s_brightness_value = nullptr;
    s_auto_rotate_status = nullptr;
    s_power_card = nullptr;
    s_power_battery = nullptr;
    s_power_battery_detail = nullptr;
    s_power_detail_overlay = nullptr;
    s_power_detail_percent = nullptr;
    s_power_detail_voltage = nullptr;
    s_power_detail_state = nullptr;
    s_power_detail_extra = nullptr;
    s_power_detail_config = nullptr;
    s_power_status_valid = false;
    s_wifi_status = nullptr;
    s_wifi_switch = nullptr;
    s_ntp_switch = nullptr;
    s_ntp_sync_button = nullptr;
    s_wifi_detail = nullptr;
    s_wifi_connected = false;
    reset_wifi_setup_pointers();
    for (size_t i = 0; i < MENU_PAGE_COUNT; ++i) {
        s_page_tabs[i] = nullptr;
        s_page_tab_icons[i] = nullptr;
        s_page_tab_labels[i] = nullptr;
    }
    for (size_t i = 0; i < SLEEP_OPTION_COUNT; ++i) {
        s_sleep_buttons[i] = nullptr;
        s_sleep_labels[i] = nullptr;
    }
}

static void menu_set_opa_cb(void *obj, int32_t value)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj), (lv_opa_t)value, 0);
}

static void menu_set_scale_cb(void *obj, int32_t value)
{
    lv_obj_set_style_transform_scale(static_cast<lv_obj_t *>(obj), value, 0);
}

static void start_menu_entrance_animation(void)
{
    lv_obj_set_style_opa(s_menu_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_transform_scale(s_menu_content, 236, 0);

    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, s_menu_content);
    lv_anim_set_exec_cb(&fade, menu_set_opa_cb);
    lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade, 160);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
    lv_anim_start(&fade);

    lv_anim_t scale;
    lv_anim_init(&scale);
    lv_anim_set_var(&scale, s_menu_content);
    lv_anim_set_exec_cb(&scale, menu_set_scale_cb);
    lv_anim_set_values(&scale, 236, 256);
    lv_anim_set_duration(&scale, 200);
    lv_anim_set_path_cb(&scale, lv_anim_path_ease_out);
    lv_anim_start(&scale);
}

static void on_menu_close_animation_done(lv_anim_t *animation)
{
    (void)animation;

    if (s_menu_root != nullptr) {
        lv_obj_delete(s_menu_root);
        s_menu_root = nullptr;
    }
    reset_menu_widget_pointers();

    s_menu_closing = false;
    clock_settings_menu_close_done_cb_t done_cb = s_close_done_cb;
    void *done_ctx = s_close_done_ctx;
    s_close_done_cb = nullptr;
    s_close_done_ctx = nullptr;
    if (done_cb != nullptr) {
        done_cb(done_ctx);
    }
}

static void on_menu_content_fade_done(lv_anim_t *animation)
{
    (void)animation;

    if (s_menu_root == nullptr) {
        return;
    }

    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, s_menu_root);
    lv_anim_set_exec_cb(&fade, menu_set_opa_cb);
    lv_anim_set_values(&fade, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&fade, 80);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&fade, on_menu_close_animation_done);
    lv_anim_start(&fade);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    return label;
}

static void style_switch(lv_obj_t *sw)
{
    const lv_style_selector_t checked_indicator =
        (lv_style_selector_t)LV_PART_INDICATOR | (lv_style_selector_t)LV_STATE_CHECKED;

    lv_obj_set_style_bg_color(sw, lv_color_hex(MENU_COLOR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(MENU_COLOR_WARM), checked_indicator);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, checked_indicator);
    lv_obj_set_style_bg_color(sw, lv_color_hex(MENU_COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
}

static void style_sleep_button(lv_obj_t *button, bool active)
{
    lv_obj_set_style_bg_color(button,
                              active ? lv_color_hex(MENU_COLOR_WARM) : lv_color_hex(MENU_COLOR_SURFACE),
                              0);
    lv_obj_set_style_bg_opa(button, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(button,
                              active ? lv_color_hex(MENU_COLOR_WARM)
                                     : lv_color_hex(MENU_COLOR_SURFACE_PRESSED),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 22, 0);
    lv_obj_set_style_border_width(button, active ? 0 : 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(MENU_COLOR_BORDER), 0);
    lv_obj_set_style_border_opa(button, active ? 0 : 100, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_outline_width(button, 0, 0);
}

static void notify_activity(void)
{
    if (s_config.activity_cb != nullptr) {
        s_config.activity_cb(s_config.user_ctx);
    }
}

static void set_label_text_if_changed(lv_obj_t *label, const char *text)
{
    if (label == nullptr || text == nullptr) {
        return;
    }
    const char *current = lv_label_get_text(label);
    if (current == nullptr || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void update_brightness_value(int brightness_percent)
{
    if (s_brightness_value == nullptr) {
        return;
    }

    char text[12] = {};
    snprintf(text, sizeof(text), "%d%%", brightness_percent);
    lv_label_set_text(s_brightness_value, text);
}

static void update_auto_rotate_status(bool enabled)
{
    if (s_auto_rotate_status == nullptr) {
        return;
    }

    lv_label_set_text(s_auto_rotate_status, enabled ? "On" : "Off");
    lv_obj_set_style_text_color(s_auto_rotate_status,
                                enabled ? lv_color_hex(MENU_COLOR_WARM)
                                        : lv_color_hex(MENU_COLOR_MUTED),
                                0);
}

static size_t sleep_index_from_value(uint32_t sleep_timeout_sec)
{
    for (size_t i = 0; i < SLEEP_OPTION_COUNT; ++i) {
        if (SLEEP_OPTIONS_SEC[i] == sleep_timeout_sec) {
            return i;
        }
    }
    return 1;
}

static void update_sleep_buttons(uint32_t sleep_timeout_sec)
{
    const size_t active_index = sleep_index_from_value(sleep_timeout_sec);
    for (size_t i = 0; i < SLEEP_OPTION_COUNT; ++i) {
        if (s_sleep_buttons[i] == nullptr) {
            continue;
        }
        const bool active = i == active_index;
        style_sleep_button(s_sleep_buttons[i], active);
        if (s_sleep_labels[i] != nullptr) {
            lv_obj_set_style_text_color(s_sleep_labels[i],
                                        active ? lv_color_hex(MENU_COLOR_WARM_TEXT)
                                               : lv_color_hex(MENU_COLOR_TEXT),
                                        0);
        }
    }
}

static void update_page_tabs(uint32_t active_index)
{
    for (uint32_t i = 0; i < MENU_PAGE_COUNT; ++i) {
        lv_obj_t *tab = s_page_tabs[i];
        if (tab == nullptr) {
            continue;
        }

        const bool active = i == active_index;
        lv_obj_set_style_bg_color(tab,
                                  active ? lv_color_hex(MENU_COLOR_TAB)
                                         : lv_color_hex(MENU_COLOR_SURFACE),
                                  0);
        lv_obj_set_style_bg_opa(tab, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(tab, 0, 0);
        lv_obj_set_style_shadow_width(tab, 0, 0);

        if (s_page_tab_icons[i] != nullptr) {
            lv_obj_set_style_text_color(s_page_tab_icons[i],
                                        active ? lv_color_hex(MENU_COLOR_TAB_TEXT)
                                               : lv_color_hex(MENU_COLOR_MUTED),
                                        0);
        }
        if (s_page_tab_labels[i] != nullptr) {
            lv_obj_set_style_text_color(s_page_tab_labels[i],
                                        active ? lv_color_hex(MENU_COLOR_TAB_TEXT)
                                               : lv_color_hex(MENU_COLOR_MUTED),
                                        0);
        }
    }
}

static void scroll_to_page(lv_obj_t *wheel, uint32_t active_index, lv_anim_enable_t anim)
{
    if (active_index >= MENU_PAGE_COUNT) {
        active_index = MENU_PAGE_COUNT - 1;
    }
    update_page_tabs(active_index);
    lv_obj_scroll_to_y(wheel, (int32_t)active_index * MENU_PAGE_STRIDE, anim);
}

static void on_page_tab_clicked(lv_event_t *event)
{
    notify_activity();
    const uintptr_t index = (uintptr_t)lv_event_get_user_data(event);
    if (index >= MENU_PAGE_COUNT || s_menu_wheel == nullptr) {
        return;
    }

    const int32_t current_index =
        (lv_obj_get_scroll_y(s_menu_wheel) + MENU_PAGE_STRIDE / 2) / MENU_PAGE_STRIDE;
    if (index != (uintptr_t)current_index) {
        s_suppress_scroll_sound_until_ms = lv_tick_get() + 600;
    }
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    set_wheel_scroll_enabled(true);
    if (s_sound_body != nullptr) {
        lv_obj_add_flag(s_sound_body, LV_OBJ_FLAG_SCROLLABLE);
    }
    scroll_to_page(s_menu_wheel, (uint32_t)index, LV_ANIM_ON);
}

static void on_wheel_scroll_end(lv_event_t *event)
{
    notify_activity();
    const uint32_t now_ms = lv_tick_get();
    if (s_suppress_scroll_sound_until_ms != 0 &&
        (int32_t)(s_suppress_scroll_sound_until_ms - now_ms) > 0) {
        s_suppress_scroll_sound_until_ms = 0;
    } else {
        s_suppress_scroll_sound_until_ms = 0;
        play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_SWIPE);
    }
    lv_obj_t *wheel = static_cast<lv_obj_t *>(lv_event_get_target(event));
    int32_t active_index = (lv_obj_get_scroll_y(wheel) + MENU_PAGE_STRIDE / 2) / MENU_PAGE_STRIDE;
    if (active_index < 0) {
        active_index = 0;
    } else if (active_index >= (int32_t)MENU_PAGE_COUNT) {
        active_index = (int32_t)MENU_PAGE_COUNT - 1;
    }
    scroll_to_page(wheel, (uint32_t)active_index, LV_ANIM_ON);
}

static void on_root_pressed(lv_event_t *event)
{
    (void)event;
    notify_activity();
}

static void set_wheel_scroll_enabled(bool enabled)
{
    if (s_menu_wheel == nullptr) {
        return;
    }

    if (enabled) {
        lv_obj_add_flag(s_menu_wheel, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_clear_flag(s_menu_wheel, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void on_brightness_pressed(lv_event_t *event)
{
    (void)event;
    notify_activity();
    // Keep slider dragging from turning into a page scroll.
    set_wheel_scroll_enabled(false);
    if (s_sound_body != nullptr) {
        lv_obj_clear_flag(s_sound_body, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void on_brightness_released(lv_event_t *event)
{
    (void)event;
    set_wheel_scroll_enabled(true);
    if (s_sound_body != nullptr) {
        lv_obj_add_flag(s_sound_body, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void on_brightness_changed(lv_event_t *event)
{
    notify_activity();
    lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const int value = (int)lv_slider_get_value(slider);
    update_brightness_value(value);
    if (s_config.brightness_cb != nullptr) {
        s_config.brightness_cb(value, s_config.user_ctx);
    }
}

static void on_auto_rotate_changed(lv_event_t *event)
{
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    update_auto_rotate_status(enabled);
    if (s_config.auto_rotate_cb != nullptr) {
        s_config.auto_rotate_cb(enabled, s_config.user_ctx);
    }
}

static void on_sleep_clicked(lv_event_t *event)
{
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    const uintptr_t index = (uintptr_t)lv_event_get_user_data(event);
    if (index >= SLEEP_OPTION_COUNT) {
        return;
    }
    const uint32_t sleep_timeout_sec = SLEEP_OPTIONS_SEC[index];
    update_sleep_buttons(sleep_timeout_sec);
    if (s_config.sleep_cb != nullptr) {
        s_config.sleep_cb(sleep_timeout_sec, s_config.user_ctx);
    }
}

static void on_close_clicked(lv_event_t *event)
{
    (void)event;
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CONFIRM);
    if (s_config.close_cb != nullptr) {
        s_config.close_cb(s_config.user_ctx);
    }
}

static lv_obj_t *make_setting_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, 300, MENU_PAGE_HEIGHT);
    lv_obj_set_flex_grow(page, 0);
    lv_obj_set_style_margin_bottom(page, MENU_PAGE_GAP, 0);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return page;
}

static lv_obj_t *make_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = make_label(parent, text, lv_color_hex(MENU_COLOR_TEXT));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    return label;
}

static lv_obj_t *make_page_header(lv_obj_t *parent, const char *symbol, const char *title)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 280, 52);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *badge = lv_obj_create(row);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 52, 52);
    lv_obj_set_style_radius(badge, 15, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(MENU_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(badge, 22, 0);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(MENU_COLOR_ACCENT), 0);
    lv_obj_set_style_border_opa(badge, 52, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *icon = lv_label_create(badge);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(MENU_COLOR_ACCENT), 0);
    lv_obj_center(icon);

    make_title(row, title);
    return row;
}

static void make_page_caption(lv_obj_t *parent, const char *text)
{
    lv_obj_t *caption = make_label(parent, text, lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(caption, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(caption, 2, 0);
}

static lv_obj_t *make_value_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = make_label(parent, text, lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
    return label;
}

static void style_small_button(lv_obj_t *button)
{
    lv_obj_set_style_radius(button, 18, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(MENU_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(MENU_COLOR_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(button, 40, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(MENU_COLOR_ACCENT), 0);
    lv_obj_set_style_border_opa(button, 70, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x153C38), LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(button, lv_color_hex(MENU_COLOR_ACCENT), LV_STATE_CHECKED);
    lv_obj_set_style_border_opa(button, LV_OPA_COVER, LV_STATE_CHECKED);
}

static lv_obj_t *make_small_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 100, 44);
    style_small_button(button);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(MENU_COLOR_ACCENT), 0);
    lv_obj_center(label);
    return button;
}

static void style_wifi_keyboard(lv_obj_t *keyboard)
{
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(keyboard, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(keyboard, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(keyboard, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_column(keyboard, 3, LV_PART_MAIN);
    lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_bg_color(keyboard, lv_color_hex(MENU_COLOR_SURFACE), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, lv_color_hex(MENU_COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_18, LV_PART_ITEMS);
    lv_obj_set_style_border_width(keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(keyboard, 10, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(keyboard, 0, LV_PART_ITEMS);

    const lv_style_selector_t pressed =
        (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_PRESSED;
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(MENU_COLOR_ACCENT), pressed);
    lv_obj_set_style_text_color(keyboard, lv_color_hex(MENU_COLOR_ACCENT_TEXT), pressed);
}

static void copy_text(char *dest, size_t dest_size, const char *src)
{
    if (dest == nullptr || dest_size == 0) {
        return;
    }
    const char *text = src != nullptr ? src : "";
    const size_t length = strnlen(text, dest_size - 1);
    memcpy(dest, text, length);
    dest[length] = '\0';
}

static lv_obj_t *make_action_button(lv_obj_t *parent, const char *text, int32_t width)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, 44);
    style_small_button(button);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(MENU_COLOR_ACCENT), 0);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *make_summary_row(lv_obj_t *parent,
                                  const char *caption,
                                  lv_obj_t **value_out)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, 320, 58);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(MENU_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(MENU_COLOR_SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(MENU_COLOR_BORDER), 0);
    lv_obj_set_style_shadow_width(row, 0, 0);

    lv_obj_t *caption_label = make_label(row, caption, lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(caption_label, &lv_font_montserrat_14, 0);
    lv_obj_align(caption_label, LV_ALIGN_TOP_LEFT, 16, 8);

    lv_obj_t *value = make_label(row, "", lv_color_hex(MENU_COLOR_TEXT));
    lv_obj_set_style_text_font(value, &lv_font_montserrat_18, 0);
    lv_obj_set_width(value, 250);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_align(value, LV_ALIGN_TOP_LEFT, 16, 27);

    lv_obj_t *chevron = make_label(row, LV_SYMBOL_RIGHT, lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(chevron, &lv_font_montserrat_16, 0);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -16, 0);

    if (value_out != nullptr) {
        *value_out = value;
    }
    return row;
}

static void close_wifi_setup(void)
{
    if (s_wifi_setup_overlay != nullptr) {
        lv_obj_delete(s_wifi_setup_overlay);
    }
    reset_wifi_setup_pointers();
}

static void set_wifi_setup_status(const char *text, bool error)
{
    if (s_wifi_setup_status == nullptr) {
        return;
    }
    set_label_text_if_changed(s_wifi_setup_status, text);
    lv_obj_set_style_text_color(s_wifi_setup_status,
                                lv_color_hex(error ? MENU_COLOR_WARM : MENU_COLOR_MUTED),
                                0);
}

static void update_wifi_setup_summary(void)
{
    if (s_wifi_summary_ssid != nullptr) {
        const char *ssid =
            s_wifi_ssid_buffer[0] != '\0' ? s_wifi_ssid_buffer : s_wifi_saved_ssid;
        set_label_text_if_changed(s_wifi_summary_ssid, ssid[0] != '\0' ? ssid : "Not set");
    }

    if (s_wifi_summary_password != nullptr) {
        const char *text = "Not set";
        if (s_wifi_password_touched) {
            text = s_wifi_password_buffer[0] != '\0' ? "********" : "Enter password";
        } else if (s_wifi_has_credentials) {
            text = "Saved password";
        }
        set_label_text_if_changed(s_wifi_summary_password, text);
    }
}

static void update_password_eye(void)
{
    if (s_wifi_input == nullptr || s_wifi_password_eye == nullptr) {
        return;
    }
    lv_textarea_set_password_mode(s_wifi_input, !s_wifi_password_visible);
    if (s_wifi_password_eye_label != nullptr) {
        lv_label_set_text(s_wifi_password_eye_label,
                          s_wifi_password_visible ? LV_SYMBOL_EYE_CLOSE
                                                  : LV_SYMBOL_EYE_OPEN);
    }
}

static const char *const *wifi_keyboard_map(void)
{
    switch (s_wifi_keyboard_layer) {
    case WIFI_KB_LAYER_UPPER:
        return WIFI_KB_UPPER_MAP;
    case WIFI_KB_LAYER_SYMBOL:
        return WIFI_KB_SYMBOL_MAP;
    default:
        return WIFI_KB_LOWER_MAP;
    }
}

static void update_wifi_keyboard_map(void)
{
    if (s_wifi_keyboard == nullptr) {
        return;
    }
    lv_buttonmatrix_set_map(s_wifi_keyboard, wifi_keyboard_map());
    lv_obj_update_layout(s_wifi_keyboard);
}

static void apply_wifi_keyboard_map_async(void *unused)
{
    (void)unused;
    update_wifi_keyboard_map();
}

static void hide_wifi_key_popover(void)
{
    if (s_wifi_key_popover != nullptr) {
        lv_obj_add_flag(s_wifi_key_popover, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_wifi_key_popover(const char *text)
{
    if (s_wifi_key_popover == nullptr || s_wifi_key_popover_label == nullptr || text == nullptr) {
        return;
    }
    lv_label_set_text(s_wifi_key_popover_label, text);
    const bool single_char = strlen(text) == 1;
    lv_obj_set_style_text_font(s_wifi_key_popover_label,
                               single_char ? &lv_font_montserrat_28
                                           : &lv_font_montserrat_16,
                               0);
    lv_obj_clear_flag(s_wifi_key_popover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_wifi_key_popover);
}

static void on_wifi_keyboard_value_changed(lv_event_t *event)
{
    lv_obj_t *keyboard = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const uint32_t id = lv_buttonmatrix_get_selected_button(keyboard);
    const char *text = lv_buttonmatrix_get_button_text(keyboard, id);
    if (text == nullptr) {
        return;
    }

    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);

    if (strcmp(text, "Aa") == 0) {
        s_wifi_keyboard_layer =
            s_wifi_keyboard_layer == WIFI_KB_LAYER_UPPER ? WIFI_KB_LAYER_LOWER
                                                         : WIFI_KB_LAYER_UPPER;
        hide_wifi_key_popover();
        lv_async_call(apply_wifi_keyboard_map_async, nullptr);
        notify_activity();
        return;
    }
    if (strcmp(text, "123") == 0) {
        s_wifi_keyboard_layer = WIFI_KB_LAYER_SYMBOL;
        hide_wifi_key_popover();
        lv_async_call(apply_wifi_keyboard_map_async, nullptr);
        notify_activity();
        return;
    }
    if (strcmp(text, "abc") == 0) {
        s_wifi_keyboard_layer = WIFI_KB_LAYER_LOWER;
        hide_wifi_key_popover();
        lv_async_call(apply_wifi_keyboard_map_async, nullptr);
        notify_activity();
        return;
    }

    show_wifi_key_popover(text);
    if (strcmp(text, "Space") == 0) {
        if (s_wifi_input != nullptr) {
            lv_textarea_add_char(s_wifi_input, ' ');
        }
    } else if (strcmp(text, LV_SYMBOL_BACKSPACE) == 0) {
        if (s_wifi_input != nullptr) {
            lv_textarea_delete_char(s_wifi_input);
        }
    } else if (strlen(text) == 1 && s_wifi_input != nullptr) {
        lv_textarea_add_char(s_wifi_input, (uint8_t)text[0]);
    }
    notify_activity();
}

static void on_wifi_keyboard_released(lv_event_t *event)
{
    (void)event;
    hide_wifi_key_popover();
}

static void show_wifi_summary(void)
{
    s_wifi_scan_waiting = false;
    s_wifi_edit_field = 0;
    if (s_wifi_summary_view != nullptr) {
        lv_obj_clear_flag(s_wifi_summary_view, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_edit_view != nullptr) {
        lv_obj_add_flag(s_wifi_edit_view, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_scan_view != nullptr) {
        lv_obj_add_flag(s_wifi_scan_view, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_setup_status != nullptr) {
        lv_obj_align(s_wifi_setup_status, LV_ALIGN_TOP_MID, 0, 300);
        set_wifi_setup_status("", false);
    }
    update_wifi_setup_summary();
}

static void show_wifi_edit(uint8_t field)
{
    s_wifi_scan_waiting = false;
    if (s_wifi_input == nullptr || s_wifi_edit_view == nullptr) {
        return;
    }

    s_wifi_edit_field = field;
    const bool password = field != 0;

    if (s_wifi_summary_view != nullptr) {
        lv_obj_add_flag(s_wifi_summary_view, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_scan_view != nullptr) {
        lv_obj_add_flag(s_wifi_scan_view, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_wifi_edit_view, LV_OBJ_FLAG_HIDDEN);

    lv_textarea_set_password_mode(s_wifi_input, false);
    lv_textarea_set_max_length(s_wifi_input, password ? 64 : 32);
    lv_textarea_set_placeholder_text(s_wifi_input, password ? "Password" : "WiFi name");
    s_wifi_input_updating = true;
    lv_textarea_set_text(s_wifi_input,
                         password ? s_wifi_password_buffer : s_wifi_ssid_buffer);
    s_wifi_input_updating = false;
    s_wifi_password_dirty = false;
    s_wifi_password_visible = false;
    if (password) {
        lv_obj_set_width(s_wifi_input, 224);
        if (s_wifi_password_eye != nullptr) {
            lv_obj_clear_flag(s_wifi_password_eye, LV_OBJ_FLAG_HIDDEN);
        }
        update_password_eye();
    } else {
        lv_obj_set_width(s_wifi_input, 276);
        if (s_wifi_password_eye != nullptr) {
            lv_obj_add_flag(s_wifi_password_eye, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_set_style_text_font(s_wifi_input, &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_color(s_wifi_input, lv_color_hex(MENU_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(s_wifi_input, lv_color_hex(MENU_COLOR_SURFACE_PRESSED), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(s_wifi_input, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(s_wifi_input, lv_color_hex(MENU_COLOR_ACCENT), LV_STATE_FOCUSED);

    if (s_wifi_edit_title != nullptr) {
        set_label_text_if_changed(s_wifi_edit_title, password ? "Editing Password" : "Editing WiFi Name");
    }
    if (s_wifi_setup_status != nullptr) {
        lv_obj_align(s_wifi_setup_status, LV_ALIGN_TOP_MID, 0, 426);
        const char *hint = "";
        if (password && s_wifi_has_credentials && !s_wifi_password_touched) {
            hint = "Blank keeps saved password";
        }
        set_wifi_setup_status(hint, false);
    }

    s_wifi_keyboard_layer = WIFI_KB_LAYER_LOWER;
    update_wifi_keyboard_map();
    hide_wifi_key_popover();
    lv_obj_add_state(s_wifi_input, LV_STATE_FOCUSED);
    notify_activity();
}

static void on_wifi_input_changed(lv_event_t *event)
{
    (void)event;
    if (s_wifi_input_updating || s_wifi_input == nullptr) {
        return;
    }

    const char *text = lv_textarea_get_text(s_wifi_input);
    if (s_wifi_edit_field == 0) {
        copy_text(s_wifi_ssid_buffer, sizeof(s_wifi_ssid_buffer), text);
        if (!s_wifi_password_dirty && s_wifi_saved_ssid[0] != '\0') {
            if (strcmp(s_wifi_ssid_buffer, s_wifi_saved_ssid) == 0) {
                s_wifi_password_touched = false;
            } else {
                s_wifi_password_touched = true;
                s_wifi_password_buffer[0] = '\0';
            }
        }
    } else {
        copy_text(s_wifi_password_buffer, sizeof(s_wifi_password_buffer), text);
        s_wifi_password_dirty = true;
        s_wifi_password_touched = true;
    }
    notify_activity();
}

static void on_wifi_setup_cancel(lv_event_t *event)
{
    (void)event;
    close_wifi_setup();
}

static void on_wifi_summary_ssid_clicked(lv_event_t *event)
{
    (void)event;
    notify_activity();
    show_wifi_edit(0);
}

static void on_wifi_summary_password_clicked(lv_event_t *event)
{
    (void)event;
    notify_activity();
    show_wifi_edit(1);
}

static void on_wifi_edit_back(lv_event_t *event)
{
    (void)event;
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    show_wifi_summary();
}

static void show_wifi_scan(void)
{
    s_wifi_edit_field = 0;
    if (s_wifi_summary_view != nullptr) {
        lv_obj_add_flag(s_wifi_summary_view, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_edit_view != nullptr) {
        lv_obj_add_flag(s_wifi_edit_view, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_scan_view != nullptr) {
        lv_obj_clear_flag(s_wifi_scan_view, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_setup_status != nullptr) {
        lv_obj_align(s_wifi_setup_status, LV_ALIGN_TOP_MID, 0, 366);
        set_wifi_setup_status("Select a network", false);
    }
    if (s_wifi_scan_list != nullptr) {
        lv_obj_scroll_to_y(s_wifi_scan_list, 0, LV_ANIM_OFF);
    }
}

static void on_wifi_scan_row_clicked(lv_event_t *event)
{
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    const uintptr_t index = (uintptr_t)lv_event_get_user_data(event);
    if (index >= 8 || s_wifi_scan_ssids[index][0] == '\0') {
        return;
    }
    copy_text(s_wifi_ssid_buffer,
              sizeof(s_wifi_ssid_buffer),
              s_wifi_scan_ssids[index]);
    s_wifi_password_buffer[0] = '\0';
    const bool same_network =
        s_wifi_saved_ssid[0] != '\0' &&
        strcmp(s_wifi_ssid_buffer, s_wifi_saved_ssid) == 0;
    s_wifi_password_touched = !same_network && s_wifi_saved_ssid[0] != '\0';
    s_wifi_password_dirty = false;
    notify_activity();
    show_wifi_edit(1);
}

static void poll_wifi_scan(lv_timer_t *timer)
{
    (void)timer;
    if (!s_wifi_scan_waiting || s_wifi_scan_list == nullptr || s_menu_closing) {
        return;
    }
    clock_settings_menu_wifi_ap_t aps[8] = {};
    uint32_t count = 0;
    esp_err_t error = ESP_OK;
    const auto state = s_config.wifi_scan_poll_cb(aps, 8, &count, &error, s_config.user_ctx);
    if (state == CLOCK_SETTINGS_MENU_SCAN_BUSY) {
        return;
    }
    s_wifi_scan_waiting = false;
    if (s_wifi_rescan_button != nullptr) {
        lv_obj_clear_state(s_wifi_rescan_button, LV_STATE_DISABLED);
    }
    if (state != CLOCK_SETTINGS_MENU_SCAN_DONE) {
        set_wifi_setup_status(error == ESP_ERR_TIMEOUT ? "Scan timed out. Try again" :
                                                        "Scan failed. Try again", true);
        return;
    }
    count = count > 8 ? 8 : count;
    lv_obj_clean(s_wifi_scan_list);
    for (uint32_t i = 0; i < count; ++i) {
        if (aps[i].ssid[0] == '\0') {
            continue;
        }
        copy_text(s_wifi_scan_ssids[i], sizeof(s_wifi_scan_ssids[i]), aps[i].ssid);

        lv_obj_t *row = lv_button_create(s_wifi_scan_list);
        lv_obj_set_size(row, 300, 46);
        style_small_button(row);
        lv_obj_add_event_cb(row,
                            on_wifi_scan_row_clicked,
                            LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        lv_obj_t *ssid_label = make_label(row, aps[i].ssid, lv_color_hex(MENU_COLOR_TEXT));
        lv_obj_set_style_text_font(ssid_label, &lv_font_montserrat_16, 0);
        lv_obj_set_width(ssid_label, 205);
        lv_obj_set_style_text_align(ssid_label, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(ssid_label, LV_LABEL_LONG_DOT);
        lv_obj_align(ssid_label, LV_ALIGN_LEFT_MID, 14, 0);

        char rssi[16] = {};
        snprintf(rssi, sizeof(rssi), "%d dBm", aps[i].rssi);
        lv_obj_t *rssi_label = make_label(row, rssi, lv_color_hex(MENU_COLOR_MUTED));
        lv_obj_set_style_text_font(rssi_label, &lv_font_montserrat_14, 0);
        lv_obj_align(rssi_label, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    set_wifi_setup_status(count > 0 ? "Select a network" : "No networks found", count == 0);
    lv_obj_align(s_wifi_setup_status, LV_ALIGN_TOP_MID, 0, count > 0 ? 366 : 180);
}

static void start_wifi_scan(void)
{
    if (s_wifi_scan_waiting) {
        return;
    }
    if (s_config.wifi_scan_cb == nullptr || s_config.wifi_scan_poll_cb == nullptr ||
        s_wifi_scan_list == nullptr) {
        set_wifi_setup_status("Scan not available", true);
        return;
    }
    show_wifi_scan();
    lv_obj_clean(s_wifi_scan_list);
    memset(s_wifi_scan_ssids, 0, sizeof(s_wifi_scan_ssids));
    set_wifi_setup_status("Scanning...", false);
    lv_obj_align(s_wifi_setup_status, LV_ALIGN_TOP_MID, 0, 180);
    const esp_err_t err = s_config.wifi_scan_cb(s_config.user_ctx);
    if (err != ESP_OK) {
        set_wifi_setup_status("Could not start scan. Try again", true);
        return;
    }
    s_wifi_scan_waiting = true;
    if (s_wifi_rescan_button != nullptr) {
        lv_obj_add_state(s_wifi_rescan_button, LV_STATE_DISABLED);
    }
    if (s_wifi_scan_timer == nullptr) {
        s_wifi_scan_timer = lv_timer_create(poll_wifi_scan, 100, nullptr);
    }
    poll_wifi_scan(nullptr);
}

static void on_wifi_scan_clicked(lv_event_t *event)
{
    (void)event;
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    start_wifi_scan();
}

static void on_wifi_scan_back(lv_event_t *event)
{
    (void)event;
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    show_wifi_summary();
}

static void play_ui_sound(clock_settings_menu_ui_sound_t sound)
{
    if (s_config.ui_sound_cb != nullptr) {
        s_config.ui_sound_cb(sound, s_config.user_ctx);
    }
}

static const char *alarm_sound_name(uint8_t sound)
{
    switch (sound) {
    case 1:
        return "Beep";
    case 2:
        return "Melody";
    case 0:
    default:
        return "Chime";
    }
}

static const char *alarm_repeat_name(uint8_t weekdays)
{
    if (weekdays == 0) {
        return "Once";
    }
    if (weekdays == 0x7F) {
        return "Every day";
    }
    if (weekdays == 0x3E) {
        return "Weekdays";
    }
    if (weekdays == 0x41) {
        return "Weekend";
    }
    return "Custom";
}

static void style_choice_button(lv_obj_t *button, bool selected)
{
    lv_obj_set_style_bg_color(button,
                              lv_color_hex(selected ? MENU_COLOR_ACCENT : MENU_COLOR_SURFACE),
                              0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(MENU_COLOR_ACCENT), 0);
    lv_obj_set_style_border_opa(button, selected ? LV_OPA_COVER : 70, 0);
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (label != nullptr) {
        lv_obj_set_style_text_color(
            label,
            lv_color_hex(selected ? MENU_COLOR_ACCENT_TEXT : MENU_COLOR_ACCENT),
            0);
    }
}

static lv_obj_t *make_sound_overlay_shell(const char *title)
{
    lv_obj_t *overlay = lv_obj_create(s_menu_root);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, 466, 466);
    lv_obj_center(overlay);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(overlay, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(MENU_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);

    lv_obj_t *heading = make_label(overlay, title, lv_color_hex(MENU_COLOR_TEXT));
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_24, 0);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_move_foreground(overlay);
    return overlay;
}

static lv_obj_t *make_close_overlay_button(lv_obj_t *overlay, lv_event_cb_t callback)
{
    lv_obj_t *button = make_action_button(overlay, "Close", 132);
    lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 330);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    return button;
}

static void on_faces_close(lv_event_t *event)
{
    (void)event;
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    lv_obj_delete(s_faces_overlay);
    s_faces_overlay = nullptr;
    s_faces_apply = nullptr;
    for (auto &card : s_faces_cards) {
        card = nullptr;
    }
}

static void update_faces_selection(void)
{
    for (uint32_t cell = 0; cell < 4; ++cell) {
        if (s_faces_cards[cell] != nullptr) {
            if (s_faces_page * 4 + cell == s_faces_candidate) {
                lv_obj_add_state(s_faces_cards[cell], LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(s_faces_cards[cell], LV_STATE_CHECKED);
            }
        }
    }
    if (s_faces_apply != nullptr) {
        if (s_faces_submitted || s_faces_candidate == s_config.settings->theme_index ||
            s_config.theme_cb == nullptr) {
            lv_obj_add_state(s_faces_apply, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(s_faces_apply, LV_STATE_DISABLED);
        }
    }
}

static void on_face_selected(lv_event_t *event)
{
    const uint32_t index = (uintptr_t)lv_event_get_user_data(event);
    if (index >= s_config.theme_count || s_menu_closing || s_faces_submitted) {
        return;
    }
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    s_faces_candidate = index;
    update_faces_selection();
}

static void on_faces_apply(lv_event_t *event)
{
    (void)event;
    if (s_faces_overlay == nullptr || s_menu_closing || s_faces_submitted ||
        s_faces_candidate >= s_config.theme_count ||
        s_faces_candidate == s_config.settings->theme_index || s_config.theme_cb == nullptr) {
        return;
    }
    notify_activity();
    s_faces_submitted = true;
    update_faces_selection();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CONFIRM);
    s_config.theme_cb(s_faces_candidate, s_config.user_ctx);
}

static void on_faces_page(lv_event_t *event)
{
    if (s_menu_closing || s_faces_submitted) {
        return;
    }
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_SWIPE);
    clock_settings_menu_open_watch_faces((uintptr_t)lv_event_get_user_data(event));
}

void clock_settings_menu_open_watch_faces(uint32_t page)
{
    if (s_menu_root == nullptr || s_menu_closing || s_config.themes == nullptr ||
        s_config.theme_count == 0) {
        return;
    }
    if (s_faces_overlay != nullptr) {
        if (s_faces_submitted) {
            return;
        }
        lv_obj_delete(s_faces_overlay);
    } else {
        s_faces_candidate = s_config.settings->theme_index < s_config.theme_count
                                ? s_config.settings->theme_index : 0;
        s_faces_submitted = false;
    }
    s_faces_apply = nullptr;
    for (auto &card : s_faces_cards) {
        card = nullptr;
    }
    const uint32_t pages = (s_config.theme_count + 3) / 4;
    s_faces_page = page < pages ? page : pages - 1;
    s_faces_overlay = make_sound_overlay_shell("Watch faces");
    char page_text[24];
    snprintf(page_text, sizeof(page_text), "%u / %u", (unsigned)s_faces_page + 1, (unsigned)pages);
    lv_obj_t *page_label = make_label(s_faces_overlay, page_text, lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(page_label, &lv_font_montserrat_14, 0);
    lv_obj_align(page_label, LV_ALIGN_TOP_MID, 0, 60);
    for (uint32_t cell = 0; cell < 4; ++cell) {
        const uint32_t index = s_faces_page * 4 + cell;
        if (index >= s_config.theme_count) {
            break;
        }
        const auto &face = s_config.themes[index];
        lv_obj_t *card = lv_button_create(s_faces_overlay);
        s_faces_cards[cell] = card;
        lv_obj_set_size(card, 140, 126);
        lv_obj_set_style_pad_all(card, 0, 0);
        style_small_button(card);
        lv_obj_align(card, LV_ALIGN_TOP_MID, cell % 2 ? 74 : -74, 88 + (cell / 2) * 134);
        lv_obj_add_event_cb(card, on_face_selected, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
        if (face.preview != nullptr) {
            lv_obj_t *preview_clip = lv_obj_create(card);
            lv_obj_remove_style_all(preview_clip);
            lv_obj_set_size(preview_clip, 88, 88);
            lv_obj_set_style_radius(preview_clip, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_clip_corner(preview_clip, true, 0);
            lv_obj_clear_flag(preview_clip, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(preview_clip, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_align(preview_clip, LV_ALIGN_TOP_MID, 0, 8);
            lv_obj_t *preview = lv_image_create(preview_clip);
            lv_image_set_src(preview, face.preview);
            lv_obj_center(preview);
        }
        lv_obj_t *name = make_label(card, face.name, lv_color_hex(MENU_COLOR_TEXT));
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -8);
    }
    lv_obj_t *back = make_action_button(s_faces_overlay, LV_SYMBOL_LEFT, 44);
    lv_obj_align(back, LV_ALIGN_TOP_MID, -122, 368);
    lv_obj_add_event_cb(back, on_faces_page, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)(s_faces_page > 0 ? s_faces_page - 1 : 0));
    if (s_faces_page == 0) {
        lv_obj_add_state(back, LV_STATE_DISABLED);
    }
    lv_obj_t *close = make_action_button(s_faces_overlay, "Back", 88);
    lv_obj_align(close, LV_ALIGN_TOP_MID, -48, 368);
    lv_obj_add_event_cb(close, on_faces_close, LV_EVENT_CLICKED, nullptr);
    s_faces_apply = make_action_button(s_faces_overlay, "Apply", 88);
    lv_obj_align(s_faces_apply, LV_ALIGN_TOP_MID, 48, 368);
    lv_obj_add_event_cb(s_faces_apply, on_faces_apply, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *next = make_action_button(s_faces_overlay, LV_SYMBOL_RIGHT, 44);
    lv_obj_align(next, LV_ALIGN_TOP_MID, 122, 368);
    lv_obj_add_event_cb(next, on_faces_page, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)(s_faces_page + 1));
    if (s_faces_page + 1 == pages) {
        lv_obj_add_state(next, LV_STATE_DISABLED);
    }
    update_faces_selection();
}

static void on_faces_open(lv_event_t *event)
{
    (void)event;
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    clock_settings_menu_open_watch_faces(s_config.settings->theme_index / 4);
}

static lv_obj_t *make_switch_row(lv_obj_t *parent,
                                 const char *text,
                                 lv_obj_t **switch_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 280, 44);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = make_label(row, text, lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 76, 44);
    lv_obj_add_flag(sw, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    style_switch(sw);
    if (switch_out != nullptr) {
        *switch_out = sw;
    }
    return row;
}

static void close_alarm_editor(void)
{
    if (s_alarm_edit_overlay != nullptr) {
        lv_obj_delete(s_alarm_edit_overlay);
    }
    s_alarm_edit_overlay = nullptr;
    s_alarm_edit_time = nullptr;
    s_alarm_edit_sound = nullptr;
    s_alarm_edit_enable = nullptr;
    for (size_t i = 0; i < 7; ++i) {
        s_alarm_edit_weekdays[i] = nullptr;
    }
}

static void close_alarm_list(void)
{
    if (s_alarm_list_overlay != nullptr) {
        lv_obj_delete(s_alarm_list_overlay);
    }
    s_alarm_list_overlay = nullptr;
    for (size_t i = 0; i < CLOCK_ALARM_COUNT; ++i) {
        s_alarm_list_values[i] = nullptr;
        s_alarm_list_details[i] = nullptr;
    }
}

static void close_timer_overlay(void)
{
    if (s_timer_overlay != nullptr) {
        lv_obj_delete(s_timer_overlay);
    }
    s_timer_overlay = nullptr;
    s_timer_remaining = nullptr;
    s_timer_status = nullptr;
    s_timer_start_button = nullptr;
    s_timer_pause_button = nullptr;
    for (size_t i = 0; i < 4; ++i) {
        s_timer_presets[i] = nullptr;
    }
}

static void close_sleep_overlay(void)
{
    if (s_sleep_overlay != nullptr) {
        lv_obj_delete(s_sleep_overlay);
    }
    s_sleep_overlay = nullptr;
    s_sleep_status = nullptr;
    s_sleep_start_button = nullptr;
    for (size_t i = 0; i < 2; ++i) {
        s_sleep_type_buttons[i] = nullptr;
    }
    for (size_t i = 0; i < 3; ++i) {
        s_sleep_duration_buttons[i] = nullptr;
    }
}

static void update_alarm_edit_time(void)
{
    if (s_alarm_edit_time == nullptr) {
        return;
    }
    char text[16] = {};
    snprintf(text, sizeof(text), "%02u:%02u", s_alarm_edit.hour, s_alarm_edit.minute);
    set_label_text_if_changed(s_alarm_edit_time, text);
}

static void update_alarm_edit_weekdays(void)
{
    for (size_t i = 0; i < 7; ++i) {
        if (s_alarm_edit_weekdays[i] != nullptr) {
            style_choice_button(s_alarm_edit_weekdays[i],
                                (s_alarm_edit.weekdays & (1U << i)) != 0);
        }
    }
}

static void update_alarm_edit_sound(void)
{
    if (s_alarm_edit_sound != nullptr) {
        char text[32] = {};
        snprintf(text, sizeof(text), "Sound: %s", alarm_sound_name(s_alarm_edit.sound));
        lv_obj_t *label = lv_obj_get_child(s_alarm_edit_sound, 0);
        if (label != nullptr) {
            set_label_text_if_changed(label, text);
        }
    }
}

static void update_alarm_list(void)
{
    for (size_t i = 0; i < CLOCK_ALARM_COUNT; ++i) {
        if (s_alarm_list_values[i] == nullptr || s_alarm_list_details[i] == nullptr) {
            continue;
        }
        const clock_alarm_t &alarm = s_config.settings->alarms[i];
        char time_text[16] = {};
        snprintf(time_text, sizeof(time_text), "%02u:%02u", alarm.hour, alarm.minute);
        set_label_text_if_changed(s_alarm_list_values[i], time_text);
        set_label_text_if_changed(s_alarm_list_details[i],
                                  alarm.enabled ? alarm_repeat_name(alarm.weekdays) : "Off");
        lv_obj_set_style_text_color(
            s_alarm_list_details[i],
            lv_color_hex(alarm.enabled ? MENU_COLOR_ACCENT : MENU_COLOR_MUTED),
            0);
    }
}

static void update_timer_ui(void)
{
    if (s_timer_overlay == nullptr) {
        return;
    }

    for (size_t i = 0; i < 4; ++i) {
        static const uint32_t minutes[] = {5, 15, 25, 45};
        if (s_timer_presets[i] != nullptr) {
            style_choice_button(s_timer_presets[i], minutes[i] == s_timer_selected_minutes);
        }
    }

    char remaining[16] = {};
    const uint32_t shown_seconds =
        (s_timer_status_running || s_timer_status_paused)
            ? s_timer_status_remaining_sec
            : s_timer_selected_minutes * 60;
    snprintf(remaining,
             sizeof(remaining),
             "%02u:%02u",
             (unsigned)(shown_seconds / 60),
             (unsigned)(shown_seconds % 60));
    if (s_timer_remaining != nullptr) {
        set_label_text_if_changed(s_timer_remaining, remaining);
    }

    const char *status = "Ready";
    if (s_timer_status_paused) {
        status = "Paused";
    } else if (s_timer_status_running) {
        if (s_timer_status_pomodoro) {
            status = s_timer_status_break ? "Break" : "Focus";
        } else {
            status = "Running";
        }
    }
    if (s_timer_status != nullptr) {
        set_label_text_if_changed(s_timer_status, status);
    }
    if (s_timer_start_button != nullptr) {
        lv_obj_t *label = lv_obj_get_child(s_timer_start_button, 0);
        if (label != nullptr) {
            set_label_text_if_changed(label, s_timer_status_paused ? "Resume" : "Start");
        }
    }
}

static void update_sleep_ui(void)
{
    if (s_sleep_overlay == nullptr) {
        return;
    }

    if (s_sleep_type_buttons[0] != nullptr) {
        style_choice_button(s_sleep_type_buttons[0], !s_sleep_brown_selected);
    }
    if (s_sleep_type_buttons[1] != nullptr) {
        style_choice_button(s_sleep_type_buttons[1], s_sleep_brown_selected);
    }
    static const uint32_t durations[] = {15, 30, 60};
    for (size_t i = 0; i < 3; ++i) {
        if (s_sleep_duration_buttons[i] != nullptr) {
            style_choice_button(s_sleep_duration_buttons[i],
                                durations[i] == s_sleep_minutes_selected);
        }
    }

    const bool sound_enabled = s_config.settings != nullptr && s_config.settings->sound_enabled;
    if (s_sleep_start_button != nullptr) {
        if (s_sleep_status_usb && sound_enabled) {
            lv_obj_clear_state(s_sleep_start_button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_sleep_start_button, LV_STATE_DISABLED);
        }
    }
    if (s_sleep_status != nullptr) {
        if (!s_sleep_status_usb) {
            set_label_text_if_changed(s_sleep_status, "USB power required");
            lv_obj_set_style_text_color(s_sleep_status, lv_color_hex(MENU_COLOR_WARM), 0);
        } else if (!sound_enabled) {
            set_label_text_if_changed(s_sleep_status, "Turn Sound on to play");
            lv_obj_set_style_text_color(s_sleep_status, lv_color_hex(MENU_COLOR_WARM), 0);
        } else if (s_sleep_status_playing) {
            set_label_text_if_changed(s_sleep_status, "Playing");
            lv_obj_set_style_text_color(s_sleep_status, lv_color_hex(MENU_COLOR_ACCENT), 0);
        } else {
            set_label_text_if_changed(s_sleep_status, "Ready · USB powered");
            lv_obj_set_style_text_color(s_sleep_status, lv_color_hex(MENU_COLOR_MUTED), 0);
        }
    }
}

static void on_alarm_row_clicked(lv_event_t *event)
{
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    const uintptr_t index = (uintptr_t)lv_event_get_user_data(event);
    if (index < CLOCK_ALARM_COUNT) {
        open_alarm_editor((uint8_t)index);
    }
}

static void on_alarm_editor_close(lv_event_t *event)
{
    (void)event;
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    close_alarm_editor();
    open_alarm_list();
}

static void on_alarm_hour_delta(lv_event_t *event)
{
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    const int delta = (int)(intptr_t)lv_event_get_user_data(event);
    s_alarm_edit.hour = (uint8_t)((s_alarm_edit.hour + 24 + delta) % 24);
    update_alarm_edit_time();
}

static void on_alarm_minute_delta(lv_event_t *event)
{
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    const int delta = (int)(intptr_t)lv_event_get_user_data(event);
    s_alarm_edit.minute = (uint8_t)((s_alarm_edit.minute + 60 + delta) % 60);
    update_alarm_edit_time();
}

static void on_alarm_weekday_clicked(lv_event_t *event)
{
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    const uintptr_t index = (uintptr_t)lv_event_get_user_data(event);
    if (index < 7) {
        s_alarm_edit.weekdays ^= (uint8_t)(1U << index);
        update_alarm_edit_weekdays();
    }
}

static void on_alarm_sound_clicked(lv_event_t *event)
{
    (void)event;
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    s_alarm_edit.sound = (uint8_t)((s_alarm_edit.sound + 1) % 3);
    update_alarm_edit_sound();
}

static void on_alarm_enable_changed(lv_event_t *event)
{
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(event));
    s_alarm_edit.enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
}

static void on_alarm_save(lv_event_t *event)
{
    (void)event;
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CONFIRM);
    if (s_config.alarm_cb != nullptr) {
        s_config.alarm_cb(s_alarm_edit_index, &s_alarm_edit, s_config.user_ctx);
    }
    close_alarm_editor();
    open_alarm_list();
}

static void open_alarm_list(void)
{
    close_alarm_list();
    s_alarm_list_overlay = make_sound_overlay_shell("Alarms");

    for (size_t i = 0; i < CLOCK_ALARM_COUNT; ++i) {
        lv_obj_t *row = lv_button_create(s_alarm_list_overlay);
        lv_obj_set_size(row, 320, 54);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 68 + (int32_t)i * 62);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 16, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(MENU_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(MENU_COLOR_SURFACE_PRESSED), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(MENU_COLOR_BORDER), 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_add_event_cb(row, on_alarm_row_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *value = make_label(row, "--:--", lv_color_hex(MENU_COLOR_TEXT));
        lv_obj_set_style_text_font(value, &lv_font_montserrat_24, 0);
        lv_obj_align(value, LV_ALIGN_LEFT_MID, 16, 0);

        lv_obj_t *detail = make_label(row, "Off", lv_color_hex(MENU_COLOR_MUTED));
        lv_obj_set_style_text_font(detail, &lv_font_montserrat_14, 0);
        lv_obj_align(detail, LV_ALIGN_RIGHT_MID, -18, 0);

        s_alarm_list_values[i] = value;
        s_alarm_list_details[i] = detail;
    }

    make_close_overlay_button(s_alarm_list_overlay, [](lv_event_t *event) {
        (void)event;
        notify_activity();
        play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
        close_alarm_list();
    });
    update_alarm_list();
    notify_activity();
}

static void open_alarm_editor(uint8_t index)
{
    if (index >= CLOCK_ALARM_COUNT) {
        return;
    }
    close_alarm_list();
    close_alarm_editor();
    s_alarm_edit_index = index;
    s_alarm_edit = s_config.settings->alarms[index];

    s_alarm_edit_overlay = make_sound_overlay_shell("Alarm");
    char title[24] = {};
    snprintf(title, sizeof(title), "Alarm %u", (unsigned)(index + 1));
    lv_obj_t *heading = lv_obj_get_child(s_alarm_edit_overlay, 0);
    if (heading != nullptr) {
        set_label_text_if_changed(heading, title);
    }

    s_alarm_edit_time = make_label(s_alarm_edit_overlay, "--:--", lv_color_hex(MENU_COLOR_TEXT));
    lv_obj_set_style_text_font(s_alarm_edit_time, &lv_font_montserrat_44, 0);
    lv_obj_align(s_alarm_edit_time, LV_ALIGN_TOP_MID, 0, 62);

    const char *labels[] = {"H -", "H +", "M -", "M +"};
    const int deltas[] = {-1, 1, -1, 1};
    for (size_t i = 0; i < 4; ++i) {
        lv_obj_t *button = make_action_button(s_alarm_edit_overlay, labels[i], 74);
        lv_obj_align(button,
                     LV_ALIGN_TOP_MID,
                     (int32_t)(i % 2 == 0 ? -42 : 42),
                     120 + (int32_t)(i / 2) * 48);
        if (i < 2) {
            lv_obj_add_event_cb(button, on_alarm_hour_delta, LV_EVENT_CLICKED,
                                (void *)(intptr_t)deltas[i]);
            lv_obj_add_event_cb(button, on_alarm_hour_delta, LV_EVENT_LONG_PRESSED_REPEAT,
                                (void *)(intptr_t)deltas[i]);
        } else {
            lv_obj_add_event_cb(button, on_alarm_minute_delta, LV_EVENT_CLICKED,
                                (void *)(intptr_t)deltas[i]);
            lv_obj_add_event_cb(button, on_alarm_minute_delta, LV_EVENT_LONG_PRESSED_REPEAT,
                                (void *)(intptr_t)deltas[i]);
        }
    }

    static const char *const day_labels[7] = {"S", "M", "T", "W", "T", "F", "S"};
    for (size_t i = 0; i < 7; ++i) {
        lv_obj_t *button = make_action_button(s_alarm_edit_overlay, day_labels[i], 44);
        lv_obj_align(button,
                     LV_ALIGN_TOP_MID,
                     -144 + (int32_t)i * 48,
                     216);
        lv_obj_add_event_cb(button, on_alarm_weekday_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        s_alarm_edit_weekdays[i] = button;
    }

    s_alarm_edit_sound = make_action_button(s_alarm_edit_overlay, "Sound: Chime", 190);
    lv_obj_align(s_alarm_edit_sound, LV_ALIGN_TOP_MID, 0, 268);
    lv_obj_add_event_cb(s_alarm_edit_sound, on_alarm_sound_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *enable_row = make_switch_row(s_alarm_edit_overlay, "Enabled", &s_alarm_edit_enable);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, 320);
    if (s_alarm_edit.enabled) {
        lv_obj_add_state(s_alarm_edit_enable, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_alarm_edit_enable, on_alarm_enable_changed, LV_EVENT_VALUE_CHANGED,
                        nullptr);

    lv_obj_t *save = make_action_button(s_alarm_edit_overlay, "Save", 124);
    lv_obj_align(save, LV_ALIGN_TOP_MID, -68, 374);
    lv_obj_add_event_cb(save, on_alarm_save, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cancel = make_action_button(s_alarm_edit_overlay, "Cancel", 124);
    lv_obj_align(cancel, LV_ALIGN_TOP_MID, 68, 374);
    lv_obj_add_event_cb(cancel, on_alarm_editor_close, LV_EVENT_CLICKED, nullptr);

    update_alarm_edit_time();
    update_alarm_edit_weekdays();
    update_alarm_edit_sound();
    notify_activity();
}

static void on_timer_preset_clicked(lv_event_t *event)
{
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    s_timer_selected_minutes = (uint32_t)(uintptr_t)lv_event_get_user_data(event);
    update_timer_ui();
}

static void on_timer_pomodoro_changed(lv_event_t *event)
{
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(event));
    s_timer_pomodoro_selected = lv_obj_has_state(sw, LV_STATE_CHECKED);
    update_timer_ui();
}

static void on_timer_action(lv_event_t *event)
{
    notify_activity();
    const clock_settings_menu_timer_action_t action =
        (clock_settings_menu_timer_action_t)(uintptr_t)lv_event_get_user_data(event);
    play_ui_sound(action == CLOCK_SETTINGS_MENU_TIMER_START
                      ? CLOCK_SETTINGS_MENU_UI_SOUND_CONFIRM
                      : CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    if (s_config.timer_cb != nullptr) {
        s_config.timer_cb(action, s_timer_selected_minutes, s_timer_pomodoro_selected,
                          s_config.user_ctx);
    }
}

static void on_timer_close(lv_event_t *event)
{
    (void)event;
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    close_timer_overlay();
}

static void open_timer_overlay(void)
{
    if (s_timer_overlay != nullptr) {
        return;
    }
    s_timer_overlay = make_sound_overlay_shell("Timer");

    s_timer_remaining = make_label(s_timer_overlay, "25:00", lv_color_hex(MENU_COLOR_TEXT));
    lv_obj_set_style_text_font(s_timer_remaining, &lv_font_montserrat_44, 0);
    lv_obj_align(s_timer_remaining, LV_ALIGN_TOP_MID, 0, 58);

    s_timer_status = make_label(s_timer_overlay, "Ready", lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(s_timer_status, &lv_font_montserrat_14, 0);
    lv_obj_align(s_timer_status, LV_ALIGN_TOP_MID, 0, 108);

    static const uint32_t presets[] = {5, 15, 25, 45};
    for (size_t i = 0; i < 4; ++i) {
        char text[8] = {};
        snprintf(text, sizeof(text), "%u", (unsigned)presets[i]);
        lv_obj_t *button = make_action_button(s_timer_overlay, text, 64);
        lv_obj_align(button,
                     LV_ALIGN_TOP_MID,
                     -105 + (int32_t)i * 70,
                     140);
        lv_obj_add_event_cb(button, on_timer_preset_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)presets[i]);
        s_timer_presets[i] = button;
    }

    lv_obj_t *pomodoro_row = lv_obj_create(s_timer_overlay);
    lv_obj_remove_style_all(pomodoro_row);
    lv_obj_set_size(pomodoro_row, 240, 44);
    lv_obj_align(pomodoro_row, LV_ALIGN_TOP_MID, 0, 184);
    lv_obj_set_flex_flow(pomodoro_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pomodoro_row,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(pomodoro_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *pomodoro_label = make_label(pomodoro_row, "Pomodoro 25/5",
                                          lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(pomodoro_label, &lv_font_montserrat_14, 0);
    lv_obj_t *pomodoro_switch = lv_switch_create(pomodoro_row);
    lv_obj_set_size(pomodoro_switch, 76, 44);
    style_switch(pomodoro_switch);
    if (s_timer_pomodoro_selected) {
        lv_obj_add_state(pomodoro_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(pomodoro_switch, on_timer_pomodoro_changed, LV_EVENT_VALUE_CHANGED,
                        nullptr);

    s_timer_start_button = make_action_button(s_timer_overlay, "Start", 82);
    lv_obj_align(s_timer_start_button, LV_ALIGN_TOP_MID, -94, 240);
    lv_obj_add_event_cb(s_timer_start_button, on_timer_action, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)CLOCK_SETTINGS_MENU_TIMER_START);
    s_timer_pause_button = make_action_button(s_timer_overlay, "Pause", 82);
    lv_obj_align(s_timer_pause_button, LV_ALIGN_TOP_MID, 0, 240);
    lv_obj_add_event_cb(s_timer_pause_button, on_timer_action, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)CLOCK_SETTINGS_MENU_TIMER_PAUSE);
    lv_obj_t *reset = make_action_button(s_timer_overlay, "Reset", 82);
    lv_obj_align(reset, LV_ALIGN_TOP_MID, 94, 240);
    lv_obj_add_event_cb(reset, on_timer_action, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)CLOCK_SETTINGS_MENU_TIMER_STOP);

    make_close_overlay_button(s_timer_overlay, on_timer_close);
    update_timer_ui();
    notify_activity();
}

static void on_sleep_type_clicked(lv_event_t *event)
{
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    s_sleep_brown_selected = (uintptr_t)lv_event_get_user_data(event) != 0;
    update_sleep_ui();
}

static void on_sleep_duration_clicked(lv_event_t *event)
{
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    s_sleep_minutes_selected = (uint32_t)(uintptr_t)lv_event_get_user_data(event);
    update_sleep_ui();
}

static void on_sleep_action(lv_event_t *event)
{
    notify_activity();
    const bool start = (uintptr_t)lv_event_get_user_data(event) != 0;
    play_ui_sound(start ? CLOCK_SETTINGS_MENU_UI_SOUND_CONFIRM
                        : CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    if (s_config.sleep_noise_cb != nullptr) {
        s_config.sleep_noise_cb(s_sleep_brown_selected, s_sleep_minutes_selected, start,
                                s_config.user_ctx);
    }
}

static void on_sleep_close(lv_event_t *event)
{
    (void)event;
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    close_sleep_overlay();
}

static void open_sleep_overlay(void)
{
    if (s_sleep_overlay != nullptr) {
        return;
    }
    s_sleep_overlay = make_sound_overlay_shell("Sleep noise");

    lv_obj_t *white = make_action_button(s_sleep_overlay, "White", 108);
    lv_obj_align(white, LV_ALIGN_TOP_MID, -62, 80);
    lv_obj_add_event_cb(white, on_sleep_type_clicked, LV_EVENT_CLICKED, (void *)0);
    s_sleep_type_buttons[0] = white;
    lv_obj_t *brown = make_action_button(s_sleep_overlay, "Brown", 108);
    lv_obj_align(brown, LV_ALIGN_TOP_MID, 62, 80);
    lv_obj_add_event_cb(brown, on_sleep_type_clicked, LV_EVENT_CLICKED, (void *)1);
    s_sleep_type_buttons[1] = brown;

    static const uint32_t durations[] = {15, 30, 60};
    for (size_t i = 0; i < 3; ++i) {
        char text[8] = {};
        snprintf(text, sizeof(text), "%u min", (unsigned)durations[i]);
        lv_obj_t *button = make_action_button(s_sleep_overlay, text, 86);
        lv_obj_align(button,
                     LV_ALIGN_TOP_MID,
                     -96 + (int32_t)i * 96,
                     144);
        lv_obj_add_event_cb(button, on_sleep_duration_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)durations[i]);
        s_sleep_duration_buttons[i] = button;
    }

    lv_obj_t *start = make_action_button(s_sleep_overlay, "Start", 118);
    s_sleep_start_button = start;
    lv_obj_align(start, LV_ALIGN_TOP_MID, -66, 208);
    lv_obj_add_event_cb(start, on_sleep_action, LV_EVENT_CLICKED, (void *)1);
    lv_obj_t *stop = make_action_button(s_sleep_overlay, "Stop", 118);
    lv_obj_align(stop, LV_ALIGN_TOP_MID, 66, 208);
    lv_obj_add_event_cb(stop, on_sleep_action, LV_EVENT_CLICKED, (void *)0);

    s_sleep_status = make_label(s_sleep_overlay, "", lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(s_sleep_status, &lv_font_montserrat_14, 0);
    lv_obj_align(s_sleep_status, LV_ALIGN_TOP_MID, 0, 266);

    make_close_overlay_button(s_sleep_overlay, on_sleep_close);
    update_sleep_ui();
    notify_activity();
}


static void on_audio_test_item_clicked(lv_event_t *event)
{
    notify_activity();
    const uint8_t sound = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    if (s_config.test_sound_cb != nullptr) {
        s_config.test_sound_cb(sound, s_config.user_ctx);
    }
}

static void on_audio_test_close(lv_event_t *event)
{
    (void)event;
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    if (s_audio_test_overlay != nullptr) {
        lv_obj_delete(s_audio_test_overlay);
    }
    s_audio_test_overlay = nullptr;
}

static void open_audio_test_overlay(void)
{
    if (s_audio_test_overlay != nullptr) {
        return;
    }
    s_audio_test_overlay = make_sound_overlay_shell("Audio test");

    static const char *const labels[] = {
        "Click", "Confirm", "Error", "Wake", "Sleep", "Chime",
        "Timer", "Alarm bell", "Alarm beep", "Alarm tune", "1 kHz",
    };
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        lv_obj_t *button = make_action_button(s_audio_test_overlay, labels[i], 138);
        lv_obj_set_height(button, 44);
        lv_obj_align(button,
                     LV_ALIGN_TOP_MID,
                     (i % 2 == 0) ? -78 : 78,
                     56 + (int32_t)(i / 2) * 46);
        lv_obj_add_event_cb(button, on_audio_test_item_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }

    make_close_overlay_button(s_audio_test_overlay, on_audio_test_close);
    notify_activity();
}

static void on_sound_master_changed(lv_event_t *event)
{
    notify_activity();
    lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    if (s_config.sound_enabled_cb != nullptr) {
        s_config.sound_enabled_cb(enabled, s_config.user_ctx);
    }
}

static void on_sound_volume_changed(lv_event_t *event)
{
    notify_activity();
    lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const uint8_t volume = (uint8_t)lv_slider_get_value(slider);
    if (s_sound_volume_value != nullptr) {
        char text[8] = {};
        snprintf(text, sizeof(text), "%u%%", (unsigned)volume);
        set_label_text_if_changed(s_sound_volume_value, text);
    }
    if (s_config.volume_cb != nullptr) {
        s_config.volume_cb(volume, s_config.user_ctx);
    }
}

static void on_sound_volume_released(lv_event_t *event)
{
    on_brightness_released(event);
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CONFIRM);
    if (s_config.test_sound_cb != nullptr) {
        s_config.test_sound_cb(CLOCK_SETTINGS_MENU_TEST_CONFIRM, s_config.user_ctx);
    }
}

static void on_test_sound_clicked(lv_event_t *event)
{
    (void)event;
    notify_activity();
    open_audio_test_overlay();
}

static void on_ui_sounds_changed(lv_event_t *event)
{
    notify_activity();
    lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (s_config.ui_sounds_cb != nullptr) {
        s_config.ui_sounds_cb(enabled, s_config.user_ctx);
    }
    if (enabled) {
        play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    }
}

static void on_chime_changed(lv_event_t *event)
{
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(event));
    if (s_config.chime_cb != nullptr) {
        s_config.chime_cb(lv_obj_has_state(sw, LV_STATE_CHECKED), s_config.user_ctx);
    }
}

static void on_alarm_open_clicked(lv_event_t *event)
{
    (void)event;
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    open_alarm_list();
}

static void on_timer_open_clicked(lv_event_t *event)
{
    (void)event;
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    open_timer_overlay();
}

static void on_sleep_open_clicked(lv_event_t *event)
{
    (void)event;
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    open_sleep_overlay();
}

static void on_wifi_scan_manual(lv_event_t *event)
{
    (void)event;
    notify_activity();
    show_wifi_edit(0);
}

static void on_wifi_edit_done(lv_event_t *event)
{
    (void)event;
    if (s_wifi_input == nullptr) {
        return;
    }

    if (s_wifi_edit_field == 0) {
        copy_text(s_wifi_ssid_buffer,
                  sizeof(s_wifi_ssid_buffer),
                  lv_textarea_get_text(s_wifi_input));
        if (s_wifi_ssid_buffer[0] == '\0') {
            play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_ERROR);
            set_wifi_setup_status("Enter WiFi name", true);
            return;
        }
    } else if (s_wifi_password_dirty) {
        copy_text(s_wifi_password_buffer,
                  sizeof(s_wifi_password_buffer),
                  lv_textarea_get_text(s_wifi_input));
        s_wifi_password_touched = true;
    }

    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CONFIRM);
    show_wifi_summary();
}

static void on_wifi_password_eye_clicked(lv_event_t *event)
{
    (void)event;
    s_wifi_password_visible = !s_wifi_password_visible;
    update_password_eye();
    notify_activity();
}

static void on_wifi_setup_connect(lv_event_t *event)
{
    (void)event;

    if (s_wifi_ssid_buffer[0] == '\0' && s_wifi_saved_ssid[0] != '\0') {
        copy_text(s_wifi_ssid_buffer, sizeof(s_wifi_ssid_buffer), s_wifi_saved_ssid);
    }
    if (s_wifi_ssid_buffer[0] == '\0') {
        play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_ERROR);
        set_wifi_setup_status("Enter WiFi name", true);
        return;
    }

    const char *password =
        s_wifi_password_touched ? s_wifi_password_buffer : nullptr;
    bool accepted = true;
    if (s_config.wifi_setup_cb != nullptr) {
        accepted = s_config.wifi_setup_cb(s_wifi_ssid_buffer, password, s_config.user_ctx);
    }
    if (!accepted) {
        play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_ERROR);
        set_wifi_setup_status("Could not save WiFi settings", true);
        return;
    }
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CONFIRM);

    copy_text(s_wifi_saved_ssid, sizeof(s_wifi_saved_ssid), s_wifi_ssid_buffer);
    s_wifi_has_credentials = true;
    s_wifi_connected = false;
    if (s_wifi_status != nullptr) {
        set_label_text_if_changed(s_wifi_status, "Connecting...");
    }
    close_wifi_setup();
}

static void on_wifi_setup_clicked(lv_event_t *event)
{
    (void)event;
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    open_wifi_setup();
}

static void open_wifi_setup(void)
{
    if (s_wifi_setup_overlay != nullptr || s_menu_root == nullptr) {
        return;
    }

    copy_text(s_wifi_ssid_buffer, sizeof(s_wifi_ssid_buffer), s_wifi_saved_ssid);
    s_wifi_password_buffer[0] = '\0';
    s_wifi_password_touched = false;
    s_wifi_password_dirty = false;
    s_wifi_password_visible = false;

    s_wifi_setup_overlay = lv_obj_create(s_menu_root);
    lv_obj_remove_style_all(s_wifi_setup_overlay);
    lv_obj_set_size(s_wifi_setup_overlay, 466, 466);
    lv_obj_center(s_wifi_setup_overlay);
    lv_obj_add_flag(s_wifi_setup_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_wifi_setup_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_wifi_setup_overlay, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_wifi_setup_overlay, lv_color_hex(MENU_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_wifi_setup_overlay, LV_OPA_COVER, 0);

    s_wifi_summary_view = lv_obj_create(s_wifi_setup_overlay);
    lv_obj_remove_style_all(s_wifi_summary_view);
    lv_obj_set_size(s_wifi_summary_view, 466, 466);
    lv_obj_center(s_wifi_summary_view);
    lv_obj_clear_flag(s_wifi_summary_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wifi_summary_view, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *summary_title =
        make_label(s_wifi_summary_view, "WiFi Setup", lv_color_hex(MENU_COLOR_TEXT));
    lv_obj_set_style_text_font(summary_title, &lv_font_montserrat_24, 0);
    lv_obj_align(summary_title, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_t *scan_button = make_small_button(s_wifi_summary_view, "Scan networks");
    lv_obj_set_size(scan_button, 180, 44);
    lv_obj_align(scan_button, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_add_event_cb(scan_button, on_wifi_scan_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *ssid_row =
        make_summary_row(s_wifi_summary_view, "WiFi Name", &s_wifi_summary_ssid);
    lv_obj_align(ssid_row, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_add_event_cb(ssid_row, on_wifi_summary_ssid_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *password_row =
        make_summary_row(s_wifi_summary_view, "Password", &s_wifi_summary_password);
    lv_obj_align(password_row, LV_ALIGN_TOP_MID, 0, 172);
    lv_obj_add_event_cb(password_row, on_wifi_summary_password_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *cancel_button = make_action_button(s_wifi_summary_view, "Cancel", 130);
    lv_obj_align(cancel_button, LV_ALIGN_TOP_MID, -75, 250);
    lv_obj_add_event_cb(cancel_button, on_wifi_setup_cancel, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *connect_button = make_action_button(s_wifi_summary_view, "Connect", 130);
    lv_obj_align(connect_button, LV_ALIGN_TOP_MID, 75, 250);
    lv_obj_add_event_cb(connect_button, on_wifi_setup_connect, LV_EVENT_CLICKED, nullptr);

    s_wifi_edit_view = lv_obj_create(s_wifi_setup_overlay);
    lv_obj_remove_style_all(s_wifi_edit_view);
    lv_obj_set_size(s_wifi_edit_view, 466, 466);
    lv_obj_center(s_wifi_edit_view);
    lv_obj_clear_flag(s_wifi_edit_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wifi_edit_view, LV_OBJ_FLAG_CLICKABLE);

    s_wifi_edit_title =
        make_label(s_wifi_edit_view, "Editing WiFi Name", lv_color_hex(MENU_COLOR_TEXT));
    lv_obj_set_style_text_font(s_wifi_edit_title, &lv_font_montserrat_20, 0);
    lv_obj_align(s_wifi_edit_title, LV_ALIGN_TOP_MID, 0, 26);

    lv_obj_t *edit_row = lv_obj_create(s_wifi_edit_view);
    lv_obj_remove_style_all(edit_row);
    lv_obj_set_size(edit_row, 276, 44);
    lv_obj_align(edit_row, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_flex_flow(edit_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(edit_row,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(edit_row, 8, 0);
    lv_obj_clear_flag(edit_row, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_input = lv_textarea_create(edit_row);
    lv_obj_set_size(s_wifi_input, 276, 44);
    lv_textarea_set_one_line(s_wifi_input, true);
    lv_textarea_set_placeholder_text(s_wifi_input, "WiFi name");
    lv_obj_set_style_text_font(s_wifi_input, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_wifi_input, lv_color_hex(MENU_COLOR_TEXT), 0);
    lv_obj_set_style_text_color(s_wifi_input,
                                lv_color_hex(MENU_COLOR_MUTED),
                                LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(s_wifi_input, lv_color_hex(MENU_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_wifi_input, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_wifi_input, 16, 0);
    lv_obj_set_style_border_width(s_wifi_input, 1, 0);
    lv_obj_set_style_border_color(s_wifi_input, lv_color_hex(MENU_COLOR_BORDER), 0);
    lv_obj_set_style_pad_left(s_wifi_input, 14, 0);
    lv_obj_set_style_pad_right(s_wifi_input, 14, 0);
    lv_obj_add_event_cb(s_wifi_input, on_wifi_input_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    s_wifi_password_eye = lv_button_create(edit_row);
    lv_obj_set_size(s_wifi_password_eye, 44, 44);
    lv_obj_set_style_radius(s_wifi_password_eye, 20, 0);
    lv_obj_set_style_bg_color(s_wifi_password_eye, lv_color_hex(MENU_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(s_wifi_password_eye,
                              lv_color_hex(MENU_COLOR_SURFACE_PRESSED),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_wifi_password_eye, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wifi_password_eye, 1, 0);
    lv_obj_set_style_border_color(s_wifi_password_eye, lv_color_hex(MENU_COLOR_BORDER), 0);
    lv_obj_set_style_shadow_width(s_wifi_password_eye, 0, 0);
    s_wifi_password_eye_label = lv_label_create(s_wifi_password_eye);
    lv_label_set_text(s_wifi_password_eye_label, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_font(s_wifi_password_eye_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_wifi_password_eye_label, lv_color_hex(MENU_COLOR_ACCENT), 0);
    lv_obj_center(s_wifi_password_eye_label);
    lv_obj_add_event_cb(s_wifi_password_eye, on_wifi_password_eye_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_wifi_password_eye, LV_OBJ_FLAG_HIDDEN);

    s_wifi_keyboard = lv_buttonmatrix_create(s_wifi_edit_view);
    lv_obj_set_size(s_wifi_keyboard, 350, 270);
    lv_obj_align(s_wifi_keyboard, LV_ALIGN_TOP_MID, 0, 104);
    style_wifi_keyboard(s_wifi_keyboard);
    lv_buttonmatrix_set_map(s_wifi_keyboard, WIFI_KB_LOWER_MAP);
    lv_obj_add_event_cb(s_wifi_keyboard,
                        on_wifi_keyboard_value_changed,
                        LV_EVENT_VALUE_CHANGED,
                        nullptr);
    lv_obj_add_event_cb(s_wifi_keyboard,
                        on_wifi_keyboard_released,
                        LV_EVENT_RELEASED,
                        nullptr);
    lv_obj_add_event_cb(s_wifi_keyboard,
                        on_wifi_keyboard_released,
                        LV_EVENT_PRESS_LOST,
                        nullptr);

    lv_obj_t *back_button = make_action_button(s_wifi_edit_view, "Back", 130);
    lv_obj_align(back_button, LV_ALIGN_TOP_MID, -75, 380);
    lv_obj_add_event_cb(back_button, on_wifi_edit_back, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *done_button = make_action_button(s_wifi_edit_view, "Done", 130);
    lv_obj_align(done_button, LV_ALIGN_TOP_MID, 75, 380);
    lv_obj_add_event_cb(done_button, on_wifi_edit_done, LV_EVENT_CLICKED, nullptr);

    s_wifi_scan_view = lv_obj_create(s_wifi_setup_overlay);
    lv_obj_remove_style_all(s_wifi_scan_view);
    lv_obj_set_size(s_wifi_scan_view, 466, 466);
    lv_obj_center(s_wifi_scan_view);
    lv_obj_clear_flag(s_wifi_scan_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wifi_scan_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_wifi_scan_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *scan_title =
        make_label(s_wifi_scan_view, "Nearby Networks", lv_color_hex(MENU_COLOR_TEXT));
    lv_obj_set_style_text_font(scan_title, &lv_font_montserrat_22, 0);
    lv_obj_align(scan_title, LV_ALIGN_TOP_MID, 0, 36);

    s_wifi_scan_list = lv_obj_create(s_wifi_scan_view);
    lv_obj_remove_style_all(s_wifi_scan_list);
    lv_obj_set_size(s_wifi_scan_list, 320, 210);
    lv_obj_align(s_wifi_scan_list, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_flex_flow(s_wifi_scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_wifi_scan_list,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_wifi_scan_list, 4, 0);
    lv_obj_set_scroll_dir(s_wifi_scan_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_wifi_scan_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_wifi_scan_list, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *manual_button = make_action_button(s_wifi_scan_view, "Manual", 92);
    lv_obj_align(manual_button, LV_ALIGN_TOP_MID, -102, 316);
    lv_obj_add_event_cb(manual_button, on_wifi_scan_manual, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *scan_back_button = make_action_button(s_wifi_scan_view, "Back", 92);
    lv_obj_align(scan_back_button, LV_ALIGN_TOP_MID, 102, 316);
    lv_obj_add_event_cb(scan_back_button, on_wifi_scan_back, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *rescan = make_action_button(s_wifi_scan_view, "Rescan", 96);
    s_wifi_rescan_button = rescan;
    lv_obj_align(rescan, LV_ALIGN_TOP_MID, 0, 316);
    lv_obj_add_event_cb(rescan, on_wifi_scan_clicked, LV_EVENT_CLICKED, nullptr);

    s_wifi_key_popover = lv_obj_create(s_wifi_edit_view);
    lv_obj_remove_style_all(s_wifi_key_popover);
    lv_obj_set_size(s_wifi_key_popover, 52, 52);
    lv_obj_align(s_wifi_key_popover, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_clear_flag(s_wifi_key_popover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_wifi_key_popover, 26, 0);
    lv_obj_set_style_bg_color(s_wifi_key_popover, lv_color_hex(MENU_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_wifi_key_popover, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wifi_key_popover, 2, 0);
    lv_obj_set_style_border_color(s_wifi_key_popover, lv_color_hex(MENU_COLOR_ACCENT), 0);
    lv_obj_set_style_shadow_width(s_wifi_key_popover, 0, 0);
    s_wifi_key_popover_label = lv_label_create(s_wifi_key_popover);
    lv_label_set_text(s_wifi_key_popover_label, "");
    lv_obj_set_style_text_font(s_wifi_key_popover_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_wifi_key_popover_label, lv_color_hex(MENU_COLOR_TEXT), 0);
    lv_obj_center(s_wifi_key_popover_label);
    lv_obj_add_flag(s_wifi_key_popover, LV_OBJ_FLAG_HIDDEN);

    s_wifi_setup_status =
        make_label(s_wifi_setup_overlay, "", lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(s_wifi_setup_status, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_wifi_setup_status, 280);
    lv_obj_set_style_text_align(s_wifi_setup_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_wifi_setup_status, LV_LABEL_LONG_DOT);
    lv_obj_align(s_wifi_setup_status, LV_ALIGN_TOP_MID, 0, 426);

    show_wifi_summary();
    lv_obj_move_foreground(s_wifi_setup_status);
    lv_obj_move_foreground(s_wifi_setup_overlay);
    notify_activity();
}

static void on_wifi_changed(lv_event_t *event)
{
    if (s_updating_wifi_ui) {
        return;
    }

    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (enabled && !s_wifi_has_credentials) {
        s_updating_wifi_ui = true;
        lv_obj_clear_state(sw, LV_STATE_CHECKED);
        s_updating_wifi_ui = false;
        open_wifi_setup();
        return;
    }

    if (s_config.wifi_cb != nullptr) {
        s_config.wifi_cb(enabled, s_config.user_ctx);
    }
}

static void on_ntp_changed(lv_event_t *event)
{
    if (s_updating_wifi_ui) {
        return;
    }

    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (s_config.ntp_cb != nullptr) {
        s_config.ntp_cb(enabled, s_config.user_ctx);
    }
}

static void on_ntp_sync_clicked(lv_event_t *event)
{
    (void)event;
    notify_activity();
    play_ui_sound(CLOCK_SETTINGS_MENU_UI_SOUND_CLICK);
    if (!s_wifi_connected) {
        return;
    }
    if (s_config.ntp_sync_cb != nullptr) {
        s_config.ntp_sync_cb(s_config.user_ctx);
    }
}

lv_obj_t *clock_settings_menu_create(lv_obj_t *parent, const clock_settings_menu_config_t *config)
{
    if (parent == nullptr || config == nullptr || config->settings == nullptr) {
        return nullptr;
    }

    s_config = *config;
    s_menu_content = nullptr;
    s_menu_wheel = nullptr;
    s_brightness_value = nullptr;
    s_auto_rotate_status = nullptr;
    s_power_card = nullptr;
    s_power_battery = nullptr;
    s_power_battery_detail = nullptr;
    s_power_detail_overlay = nullptr;
    s_power_detail_percent = nullptr;
    s_power_detail_voltage = nullptr;
    s_power_detail_state = nullptr;
    s_power_detail_extra = nullptr;
    s_power_detail_config = nullptr;
    s_power_status_valid = false;
    s_wifi_status = nullptr;
    s_wifi_switch = nullptr;
    s_ntp_switch = nullptr;
    s_ntp_sync_button = nullptr;
    s_wifi_detail = nullptr;
    s_wifi_has_credentials = false;
    s_wifi_connected = false;
    reset_wifi_setup_pointers();
    s_menu_closing = false;
    s_close_done_cb = nullptr;
    s_close_done_ctx = nullptr;
    for (size_t i = 0; i < MENU_PAGE_COUNT; ++i) {
        s_page_tabs[i] = nullptr;
        s_page_tab_icons[i] = nullptr;
        s_page_tab_labels[i] = nullptr;
    }
    for (size_t i = 0; i < SLEEP_OPTION_COUNT; ++i) {
        s_sleep_buttons[i] = nullptr;
        s_sleep_labels[i] = nullptr;
    }

    s_menu_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_menu_root);
    lv_obj_set_size(s_menu_root, 466, 466);
    lv_obj_center(s_menu_root);
    lv_obj_add_flag(s_menu_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_menu_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_menu_root, lv_color_hex(MENU_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_menu_root, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_menu_root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_menu_root, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(s_menu_root, on_root_pressed, LV_EVENT_PRESSED, nullptr);

    lv_obj_t *content = lv_obj_create(s_menu_root);
    s_menu_content = content;
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, 334, 400);
    lv_obj_center(content);
    lv_obj_set_style_transform_pivot_x(content, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(content, lv_pct(50), 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // Only Done or the physical button closes the menu.
    lv_obj_add_flag(content, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = make_label(content, "SETTINGS", lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_set_style_margin_bottom(title, 4, 0);

    lv_obj_t *close_button = lv_button_create(content);
    lv_obj_set_size(close_button, 96, 44);
    lv_obj_set_style_margin_bottom(close_button, 8, 0);
    lv_obj_set_style_radius(close_button, 18, 0);
    lv_obj_set_style_bg_color(close_button, lv_color_hex(MENU_COLOR_WARM), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(close_button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(close_button, 30, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(close_button, 1, 0);
    lv_obj_set_style_border_color(close_button, lv_color_hex(MENU_COLOR_WARM), 0);
    lv_obj_set_style_border_opa(close_button, 150, 0);
    lv_obj_set_style_shadow_width(close_button, 0, 0);
    lv_obj_add_event_cb(close_button, on_close_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *close_label = lv_label_create(close_button);
    lv_label_set_text(close_label, "Done");
    lv_obj_set_style_text_color(close_label, lv_color_hex(MENU_COLOR_WARM), 0);
    lv_obj_set_style_text_font(close_label, &lv_font_montserrat_16, 0);
    lv_obj_center(close_label);

    lv_obj_t *wheel = lv_obj_create(content);
    s_menu_wheel = wheel;
    lv_obj_remove_style_all(wheel);
    lv_obj_set_size(wheel, 300, MENU_PAGE_HEIGHT);
    lv_obj_set_style_pad_top(wheel, 0, 0);
    lv_obj_set_style_pad_bottom(wheel, 0, 0);
    lv_obj_set_style_pad_left(wheel, 0, 0);
    lv_obj_set_style_pad_right(wheel, 0, 0);
    lv_obj_set_flex_flow(wheel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wheel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(wheel, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(wheel, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(wheel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(wheel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(wheel, on_wheel_scroll_end, LV_EVENT_SCROLL_END, nullptr);

    lv_obj_t *brightness_page = make_setting_page(wheel);
    make_page_caption(brightness_page, "DISPLAY");
    make_page_header(brightness_page, LV_SYMBOL_EYE_OPEN, "Brightness");

    s_brightness_value = make_label(brightness_page, "", lv_color_hex(MENU_COLOR_TEXT));
    lv_obj_set_style_text_font(s_brightness_value, &lv_font_montserrat_36, 0);
    update_brightness_value(config->settings->brightness_percent);

    lv_obj_t *brightness_slider = lv_slider_create(brightness_page);
    lv_obj_set_size(brightness_slider, 260, 30);
    lv_obj_set_style_margin_top(brightness_slider, 6, 0);
    lv_obj_set_ext_click_area(brightness_slider, 18);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(MENU_COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(brightness_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(brightness_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(MENU_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(brightness_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(brightness_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(MENU_COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(brightness_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(brightness_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_border_width(brightness_slider, 2, LV_PART_KNOB);
    lv_obj_set_style_border_color(brightness_slider, lv_color_hex(MENU_COLOR_ACCENT), LV_PART_KNOB);
    lv_obj_set_style_border_opa(brightness_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(brightness_slider, 7, LV_PART_KNOB);
    lv_slider_set_range(brightness_slider, 1, 100);
    lv_slider_set_value(brightness_slider, config->settings->brightness_percent, LV_ANIM_OFF);
    lv_obj_add_event_cb(brightness_slider, on_brightness_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(brightness_slider, on_brightness_pressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(brightness_slider, on_brightness_released, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(brightness_slider, on_brightness_released, LV_EVENT_PRESS_LOST, nullptr);

    lv_obj_t *brightness_scale = lv_obj_create(brightness_page);
    lv_obj_remove_style_all(brightness_scale);
    lv_obj_set_size(brightness_scale, 260, 16);
    lv_obj_set_style_margin_top(brightness_scale, 4, 0);
    lv_obj_set_flex_flow(brightness_scale, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brightness_scale,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(brightness_scale, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *brightness_min = make_label(brightness_scale, "1%", lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(brightness_min, &lv_font_montserrat_14, 0);
    lv_obj_t *brightness_max = make_label(brightness_scale, "100%", lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(brightness_max, &lv_font_montserrat_14, 0);

    lv_obj_t *faces_button = lv_button_create(brightness_page);
    lv_obj_set_size(faces_button, 280, 60);
    style_small_button(faces_button);
    lv_obj_set_style_pad_all(faces_button, 0, 0);
    lv_obj_set_style_margin_top(faces_button, 8, 0);
    lv_obj_add_flag(faces_button, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_add_event_cb(faces_button, on_faces_open, LV_EVENT_CLICKED, nullptr);
    const uint32_t current_face = config->settings->theme_index < config->theme_count
                                      ? config->settings->theme_index : 0;
    const clock_settings_menu_theme_t *face = config->themes != nullptr && config->theme_count > 0
                                                ? &config->themes[current_face] : nullptr;
    if (face != nullptr && face->preview != nullptr) {
        lv_obj_t *preview_clip = lv_obj_create(faces_button);
        lv_obj_remove_style_all(preview_clip);
        lv_obj_set_size(preview_clip, 44, 44);
        lv_obj_set_style_radius(preview_clip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_clip_corner(preview_clip, true, 0);
        lv_obj_clear_flag(preview_clip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(preview_clip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(preview_clip, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_t *preview = lv_image_create(preview_clip);
        lv_image_set_src(preview, face->preview);
        lv_image_set_scale(preview, 128);
        lv_obj_center(preview);
    }
    lv_obj_t *faces_label = make_label(faces_button, "Watch faces", lv_color_hex(MENU_COLOR_TEXT));
    lv_obj_set_style_text_font(faces_label, &lv_font_montserrat_16, 0);
    lv_obj_align(faces_label, LV_ALIGN_TOP_LEFT, 66, 9);
    lv_obj_t *face_name = make_label(faces_button, face != nullptr ? face->name : "Choose a style",
                                    lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(face_name, &lv_font_montserrat_14, 0);
    lv_obj_set_width(face_name, 174);
    lv_label_set_long_mode(face_name, LV_LABEL_LONG_DOT);
    lv_obj_align(face_name, LV_ALIGN_BOTTOM_LEFT, 66, -9);
    lv_obj_t *faces_arrow = make_label(faces_button, LV_SYMBOL_RIGHT, lv_color_hex(MENU_COLOR_ACCENT));
    lv_obj_align(faces_arrow, LV_ALIGN_RIGHT_MID, -14, 0);

    lv_obj_t *sleep_page = make_setting_page(wheel);
    make_page_caption(sleep_page, "POWER");
    make_page_header(sleep_page, LV_SYMBOL_POWER, "Power");

    s_power_card = lv_button_create(sleep_page);
    lv_obj_set_size(s_power_card, 280, 64);
    lv_obj_set_style_pad_all(s_power_card, 0, 0);
    lv_obj_set_style_radius(s_power_card, 16, 0);
    lv_obj_set_style_bg_color(s_power_card, lv_color_hex(MENU_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(s_power_card, lv_color_hex(MENU_COLOR_SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_power_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_power_card, 1, 0);
    lv_obj_set_style_border_color(s_power_card, lv_color_hex(MENU_COLOR_BORDER), 0);
    lv_obj_set_style_shadow_width(s_power_card, 0, 0);
    lv_obj_set_style_margin_top(s_power_card, 8, 0);
    lv_obj_add_flag(s_power_card, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_add_event_cb(s_power_card, on_power_card_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *battery_caption = make_label(s_power_card, "BATTERY", lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(battery_caption, &lv_font_montserrat_14, 0);
    lv_obj_align(battery_caption, LV_ALIGN_TOP_LEFT, 14, 5);

    s_power_battery = make_label(s_power_card, "Battery --", lv_color_hex(MENU_COLOR_TEXT));
    lv_obj_set_style_text_font(s_power_battery, &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_power_battery, 220);
    lv_obj_set_style_text_align(s_power_battery, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_power_battery, LV_LABEL_LONG_DOT);
    lv_obj_align(s_power_battery, LV_ALIGN_TOP_LEFT, 14, 20);

    s_power_battery_detail = make_label(s_power_card, "", lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(s_power_battery_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_power_battery_detail, 220);
    lv_obj_set_style_text_align(s_power_battery_detail, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_power_battery_detail, LV_LABEL_LONG_DOT);
    lv_obj_align(s_power_battery_detail, LV_ALIGN_TOP_LEFT, 14, 41);

    lv_obj_t *battery_chevron = make_label(s_power_card,
                                           LV_SYMBOL_RIGHT,
                                           lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(battery_chevron, &lv_font_montserrat_16, 0);
    lv_obj_align(battery_chevron, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_t *sleep_grid = lv_obj_create(sleep_page);
    lv_obj_remove_style_all(sleep_grid);
    lv_obj_set_size(sleep_grid, 280, 52);
    lv_obj_set_style_margin_top(sleep_grid, 8, 0);
    lv_obj_set_style_pad_column(sleep_grid, 8, 0);
    lv_obj_set_flex_flow(sleep_grid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sleep_grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (size_t i = 0; i < SLEEP_OPTION_COUNT; ++i) {
        lv_obj_t *button = lv_button_create(sleep_grid);
        lv_obj_set_size(button, 64, 44);
        lv_obj_add_flag(button, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        lv_obj_add_event_cb(button, on_sleep_clicked, LV_EVENT_CLICKED, (void *)i);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, SLEEP_OPTION_LABELS[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(MENU_COLOR_TEXT), 0);
        lv_obj_center(label);
        s_sleep_buttons[i] = button;
        s_sleep_labels[i] = label;
    }
    update_sleep_buttons(config->settings->sleep_timeout_sec);
    lv_obj_t *sleep_hint = make_value_label(sleep_page, "Screen timeout");
    lv_obj_set_style_text_font(sleep_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sleep_hint, lv_color_hex(MENU_COLOR_MUTED), 0);
    lv_obj_set_style_margin_top(sleep_hint, 8, 0);

    lv_obj_t *motion_page = make_setting_page(wheel);
    make_page_caption(motion_page, "MOTION");
    make_page_header(motion_page, LV_SYMBOL_REFRESH, "Auto Rotate");
    s_auto_rotate_status = make_value_label(motion_page, "");
    lv_obj_set_style_margin_bottom(s_auto_rotate_status, 10, 0);
    update_auto_rotate_status(config->settings->auto_rotate);
    lv_obj_t *auto_rotate_switch = lv_switch_create(motion_page);
    lv_obj_set_size(auto_rotate_switch, 120, 60);
    lv_obj_add_flag(auto_rotate_switch, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    style_switch(auto_rotate_switch);
    if (config->settings->auto_rotate) {
        lv_obj_add_state(auto_rotate_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(auto_rotate_switch, on_auto_rotate_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_t *motion_hint = make_value_label(motion_page, "Follow device tilt");
    lv_obj_set_style_text_font(motion_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(motion_hint, lv_color_hex(MENU_COLOR_MUTED), 0);
    lv_obj_set_style_margin_top(motion_hint, 10, 0);

    lv_obj_t *wifi_page = make_setting_page(wheel);
    make_page_caption(wifi_page, "NETWORK");
    make_page_header(wifi_page, LV_SYMBOL_WIFI, "WiFi");

    s_wifi_status = make_label(wifi_page,
                               config->settings->wifi_enabled ? "Starting..." : "WiFi off",
                               lv_color_hex(MENU_COLOR_TEXT));
    lv_obj_set_style_text_font(s_wifi_status, &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_wifi_status, 280);
    lv_obj_set_style_text_align(s_wifi_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_wifi_status, LV_LABEL_LONG_DOT);

    lv_obj_t *wifi_row = lv_obj_create(wifi_page);
    lv_obj_remove_style_all(wifi_row);
    lv_obj_set_size(wifi_row, 280, 46);
    lv_obj_set_style_margin_top(wifi_row, 4, 0);
    lv_obj_set_flex_flow(wifi_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi_row,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(wifi_row, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_switch = lv_switch_create(wifi_row);
    lv_obj_set_size(s_wifi_switch, 96, 44);
    lv_obj_add_flag(s_wifi_switch, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    style_switch(s_wifi_switch);
    if (config->settings->wifi_enabled) {
        lv_obj_add_state(s_wifi_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_wifi_switch, on_wifi_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *wifi_setup_button = make_small_button(wifi_row, "Setup");
    lv_obj_add_event_cb(wifi_setup_button, on_wifi_setup_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *ntp_row = lv_obj_create(wifi_page);
    lv_obj_remove_style_all(ntp_row);
    lv_obj_set_size(ntp_row, 280, 44);
    lv_obj_set_style_margin_top(ntp_row, 4, 0);
    lv_obj_set_flex_flow(ntp_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ntp_row,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ntp_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ntp_label = make_label(ntp_row, "Auto NTP", lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(ntp_label, &lv_font_montserrat_14, 0);
    s_ntp_switch = lv_switch_create(ntp_row);
    lv_obj_set_size(s_ntp_switch, 76, 44);
    lv_obj_add_flag(s_ntp_switch, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    style_switch(s_ntp_switch);
    if (config->settings->ntp_enabled) {
        lv_obj_add_state(s_ntp_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_ntp_switch, on_ntp_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    s_ntp_sync_button = make_small_button(wifi_page, "Sync now");
    lv_obj_set_style_margin_top(s_ntp_sync_button, 4, 0);
    lv_obj_add_state(s_ntp_sync_button, LV_STATE_DISABLED);
    lv_obj_set_style_opa(s_ntp_sync_button, LV_OPA_50, 0);
    lv_obj_add_event_cb(s_ntp_sync_button, on_ntp_sync_clicked, LV_EVENT_CLICKED, nullptr);

    s_wifi_detail = make_label(wifi_page,
                               "Tap Setup to configure",
                               lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(s_wifi_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_wifi_detail, 280);
    lv_obj_set_style_text_align(s_wifi_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_wifi_detail, LV_LABEL_LONG_DOT);
    lv_obj_set_style_margin_top(s_wifi_detail, 2, 0);

    lv_obj_t *sound_page = make_setting_page(wheel);
    make_page_header(sound_page, LV_SYMBOL_AUDIO, "Sound");
    lv_obj_t *sound_body = lv_obj_create(sound_page);
    s_sound_body = sound_body;
    lv_obj_remove_style_all(sound_body);
    lv_obj_set_size(sound_body, 296, 194);
    lv_obj_set_style_pad_left(sound_body, 8, 0);
    lv_obj_set_style_pad_right(sound_body, 8, 0);
    lv_obj_set_style_pad_bottom(sound_body, 8, 0);
    lv_obj_set_style_pad_row(sound_body, 8, 0);
    lv_obj_set_flex_flow(sound_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sound_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(sound_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sound_body, LV_SCROLLBAR_MODE_ON);
    lv_obj_set_style_bg_color(sound_body, lv_color_hex(MENU_COLOR_ACCENT), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(sound_body, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(sound_body, 3, LV_PART_SCROLLBAR);
    lv_obj_clear_flag(sound_body, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_add_event_cb(sound_body, on_root_pressed, LV_EVENT_SCROLL, nullptr);

    lv_obj_t *master_row = make_switch_row(sound_body, "Sound", &s_sound_master_switch);
    lv_obj_set_style_margin_top(master_row, 4, 0);
    if (config->settings->sound_enabled) {
        lv_obj_add_state(s_sound_master_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_sound_master_switch, on_sound_master_changed,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *volume_row = lv_obj_create(sound_body);
    lv_obj_remove_style_all(volume_row);
    lv_obj_set_size(volume_row, 280, 44);
    lv_obj_set_style_margin_top(volume_row, 4, 0);
    lv_obj_set_flex_flow(volume_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(volume_row,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(volume_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *volume_label = make_label(volume_row, "Volume", lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(volume_label, &lv_font_montserrat_14, 0);

    s_sound_volume_slider = lv_slider_create(volume_row);
    lv_obj_set_size(s_sound_volume_slider, 164, 24);
    lv_obj_set_ext_click_area(s_sound_volume_slider, 10);
    lv_obj_set_style_bg_color(s_sound_volume_slider, lv_color_hex(MENU_COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_sound_volume_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_sound_volume_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sound_volume_slider, lv_color_hex(MENU_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_sound_volume_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_sound_volume_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_sound_volume_slider, lv_color_hex(MENU_COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_sound_volume_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(s_sound_volume_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_sound_volume_slider, 2, LV_PART_KNOB);
    lv_obj_set_style_border_color(s_sound_volume_slider, lv_color_hex(MENU_COLOR_ACCENT), LV_PART_KNOB);
    lv_obj_set_style_border_opa(s_sound_volume_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_sound_volume_slider, 5, LV_PART_KNOB);
    lv_slider_set_range(s_sound_volume_slider, 0, 100);
    lv_slider_set_value(s_sound_volume_slider, config->settings->sound_volume, LV_ANIM_OFF);
    lv_obj_clear_flag(s_sound_volume_slider, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_add_event_cb(s_sound_volume_slider, on_brightness_pressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_sound_volume_slider, on_sound_volume_changed,
                        LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_sound_volume_slider, on_sound_volume_released,
                        LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_sound_volume_slider, on_sound_volume_released,
                        LV_EVENT_PRESS_LOST, nullptr);

    s_sound_volume_value = make_label(volume_row,
                                      "",
                                      lv_color_hex(MENU_COLOR_TEXT));
    lv_obj_set_width(s_sound_volume_value, 44);
    lv_obj_set_style_text_align(s_sound_volume_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_sound_volume_value, &lv_font_montserrat_14, 0);
    char volume_text[8] = {};
    snprintf(volume_text, sizeof(volume_text), "%u%%",
             (unsigned)config->settings->sound_volume);
    set_label_text_if_changed(s_sound_volume_value, volume_text);

    lv_obj_t *ui_sound_row = make_switch_row(sound_body, "UI sounds", &s_ui_sounds_switch);
    lv_obj_set_style_margin_top(ui_sound_row, 4, 0);
    if (config->settings->ui_sounds) {
        lv_obj_add_state(s_ui_sounds_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_ui_sounds_switch, on_ui_sounds_changed,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *chime_row = make_switch_row(sound_body, "Hourly chime", &s_chime_switch);
    lv_obj_set_style_margin_top(chime_row, 4, 0);
    if (config->settings->hourly_chime) {
        lv_obj_add_state(s_chime_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_chime_switch, on_chime_changed,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *sound_actions = lv_obj_create(sound_body);
    lv_obj_remove_style_all(sound_actions);
    lv_obj_set_size(sound_actions, 280, 44);
    lv_obj_set_style_margin_top(sound_actions, 4, 0);
    lv_obj_set_flex_flow(sound_actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sound_actions,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(sound_actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *alarms_button = make_action_button(sound_actions, "Alarms", 86);
    lv_obj_add_event_cb(alarms_button, on_alarm_open_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *timer_button = make_action_button(sound_actions, "Timer", 86);
    lv_obj_add_event_cb(timer_button, on_timer_open_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *sleep_button = make_action_button(sound_actions, "Sleep", 86);
    lv_obj_add_event_cb(sleep_button, on_sleep_open_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *test_sound = make_action_button(sound_body, "Audio test", 180);
    lv_obj_add_event_cb(test_sound, on_test_sound_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *alarm_hint = make_label(sound_body, "Alarms ring with Sound off", lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(alarm_hint, &lv_font_montserrat_14, 0);

    lv_obj_t *tabs = lv_obj_create(content);
    lv_obj_remove_style_all(tabs);
    lv_obj_set_size(tabs, 300, 62);
    lv_obj_set_style_margin_top(tabs, 0, 0);
    lv_obj_clear_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);
    for (size_t i = 0; i < MENU_PAGE_COUNT; ++i) {
        lv_obj_t *tab = lv_button_create(tabs);
        lv_obj_set_size(tab, 60, 52);
        lv_obj_set_pos(tab, MENU_TAB_X[i], MENU_TAB_Y[i]);
        lv_obj_set_style_radius(tab, 26, 0);
        lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(tab, 0, 0);
        lv_obj_set_style_shadow_width(tab, 0, 0);
        lv_obj_set_style_outline_width(tab, 0, 0);
        lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(tab, 2, 0);
        lv_obj_add_flag(tab, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        lv_obj_add_event_cb(tab, on_page_tab_clicked, LV_EVENT_CLICKED, (void *)i);
        lv_obj_t *icon = lv_label_create(tab);
        lv_label_set_text(icon, MENU_TAB_SYMBOLS[i]);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_26, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(MENU_COLOR_MUTED), 0);
        lv_obj_t *label = lv_label_create(tab);
        lv_label_set_text(label, MENU_TAB_LABELS[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(MENU_COLOR_MUTED), 0);
        s_page_tabs[i] = tab;
        s_page_tab_icons[i] = icon;
        s_page_tab_labels[i] = label;
    }
    update_page_tabs(0);
    scroll_to_page(wheel, 0, LV_ANIM_OFF);

    lv_obj_move_foreground(s_menu_root);
    start_menu_entrance_animation();
    return s_menu_root;
}

void clock_settings_menu_move_foreground(void)
{
    if (s_menu_root != nullptr) {
        lv_obj_move_foreground(s_menu_root);
    }
}

void clock_settings_menu_set_page(uint32_t page_index)
{
    // Host preview helper: select a page without simulating touch scrolling.
    if (s_menu_wheel == nullptr) {
        return;
    }
    if (page_index >= MENU_PAGE_COUNT) {
        page_index = MENU_PAGE_COUNT - 1;
    }
    scroll_to_page(s_menu_wheel, page_index, LV_ANIM_OFF);
    lv_obj_update_layout(s_menu_wheel);
}

void clock_settings_menu_open_wifi_setup(void)
{
    // Host preview helper: open the provisioning overlay without touch input.
    open_wifi_setup();
}

void clock_settings_menu_open_wifi_setup_edit(uint32_t field)
{
    // Host preview helper: open the SSID or password editor without touch input.
    if (s_wifi_setup_overlay == nullptr) {
        open_wifi_setup();
    }
    show_wifi_edit(field != 0 ? 1 : 0);
}

void clock_settings_menu_start_wifi_scan(void)
{
    // Host preview helper: run the configured scan callback without touch input.
    start_wifi_scan();
}

void clock_settings_menu_update_wifi_status(const clock_settings_menu_wifi_status_t *status)
{
    if (status == nullptr) {
        return;
    }

    s_updating_wifi_ui = true;
    s_wifi_has_credentials = status->has_credentials;
    s_wifi_connected = status->connected;
    if (status->ssid != nullptr) {
        copy_text(s_wifi_saved_ssid, sizeof(s_wifi_saved_ssid), status->ssid);
    }

    if (s_wifi_status != nullptr && status->state != nullptr) {
        set_label_text_if_changed(s_wifi_status, status->state);
    }
    if (s_wifi_detail != nullptr && status->detail != nullptr) {
        set_label_text_if_changed(s_wifi_detail, status->detail);
    }

    if (s_wifi_switch != nullptr) {
        const bool checked = lv_obj_has_state(s_wifi_switch, LV_STATE_CHECKED);
        if (status->enabled != checked) {
            if (status->enabled) {
                lv_obj_add_state(s_wifi_switch, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(s_wifi_switch, LV_STATE_CHECKED);
            }
        }
    }

    if (s_ntp_switch != nullptr) {
        const bool checked = lv_obj_has_state(s_ntp_switch, LV_STATE_CHECKED);
        if (status->ntp_enabled != checked) {
            if (status->ntp_enabled) {
                lv_obj_add_state(s_ntp_switch, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(s_ntp_switch, LV_STATE_CHECKED);
            }
        }
    }

    if (s_ntp_sync_button != nullptr) {
        const bool disabled = lv_obj_has_state(s_ntp_sync_button, LV_STATE_DISABLED);
        const bool should_disable = !status->connected;
        if (should_disable != disabled) {
            if (status->connected) {
                lv_obj_clear_state(s_ntp_sync_button, LV_STATE_DISABLED);
                lv_obj_set_style_opa(s_ntp_sync_button, LV_OPA_COVER, 0);
            } else {
                lv_obj_add_state(s_ntp_sync_button, LV_STATE_DISABLED);
                lv_obj_set_style_opa(s_ntp_sync_button, LV_OPA_50, 0);
            }
        }
    }

    update_wifi_setup_summary();
    s_updating_wifi_ui = false;
}

static void update_power_detail_labels(void)
{
    if (s_power_detail_overlay == nullptr) {
        return;
    }

    char percent[16] = "--";
    char voltage[32] = {};
    char state[32] = "Battery";
    char extra[64] = {};
    char config[64] = {};
    bool low_battery = false;
    lv_color_t state_color = lv_color_hex(MENU_COLOR_TEXT);

    if (!s_power_status_valid || !s_power_status.initialized) {
        snprintf(voltage, sizeof(voltage), "PMU unavailable");
        snprintf(state, sizeof(state), "Battery");
        snprintf(extra, sizeof(extra), "AXP2101 not detected");
        state_color = lv_color_hex(MENU_COLOR_MUTED);
    } else if (!s_power_status.battery_present) {
        snprintf(voltage, sizeof(voltage), "No battery");
        snprintf(state, sizeof(state), "%s", s_power_status.vbus_present ? "USB powered" : "No battery");
        snprintf(extra, sizeof(extra), "%s", s_power_status.vbus_present ? "USB connected" : "Battery not detected");
        snprintf(config, sizeof(config), "Charge 4.2 V / 400 mA");
        state_color = lv_color_hex(MENU_COLOR_MUTED);
    } else {
        snprintf(percent,
                 sizeof(percent),
                 "%s%u%%",
                 s_power_status.battery_percent_estimated ? "~" : "",
                 s_power_status.battery_percent);
        snprintf(voltage,
                 sizeof(voltage),
                 "%u.%02u V",
                 s_power_status.battery_mv / 1000,
                 (s_power_status.battery_mv % 1000) / 10);
        const char *state_text = s_power_status.charging ? "Charging"
                                 : s_power_status.full   ? "Full"
                                 : s_power_status.discharging ? "Discharging"
                                                              : "Standby";
        snprintf(state, sizeof(state), "%s", state_text);
        snprintf(extra, sizeof(extra), "%s", s_power_status.vbus_present ? "USB connected" : "On battery");
        snprintf(config,
                 sizeof(config),
                 "%s",
                 s_power_status.battery_percent_estimated
                     ? "Estimated | 4.2 V / 400 mA"
                     : "Charge 4.2 V / 400 mA");
        low_battery = s_power_status.discharging && s_power_status.battery_percent <= 15;
        if (low_battery) {
            snprintf(extra, sizeof(extra), "Low battery");
            state_color = lv_color_hex(MENU_COLOR_WARM);
        } else if (s_power_status.charging) {
            state_color = lv_color_hex(MENU_COLOR_ACCENT);
        }
    }

    if (s_power_detail_percent != nullptr) {
        set_label_text_if_changed(s_power_detail_percent, percent);
    }
    if (s_power_detail_voltage != nullptr) {
        set_label_text_if_changed(s_power_detail_voltage, voltage);
    }
    if (s_power_detail_state != nullptr) {
        set_label_text_if_changed(s_power_detail_state, state);
        lv_obj_set_style_text_color(s_power_detail_state, state_color, 0);
    }
    if (s_power_detail_extra != nullptr) {
        set_label_text_if_changed(s_power_detail_extra, extra);
        lv_obj_set_style_text_color(s_power_detail_extra,
                                    lv_color_hex(low_battery ? MENU_COLOR_WARM : MENU_COLOR_MUTED),
                                    0);
    }
    if (s_power_detail_config != nullptr) {
        set_label_text_if_changed(s_power_detail_config, config);
    }
}

static void close_power_detail(void)
{
    if (s_power_detail_overlay != nullptr) {
        lv_obj_delete(s_power_detail_overlay);
    }
    s_power_detail_overlay = nullptr;
    s_power_detail_percent = nullptr;
    s_power_detail_voltage = nullptr;
    s_power_detail_state = nullptr;
    s_power_detail_extra = nullptr;
    s_power_detail_config = nullptr;
}

static void on_power_detail_close(lv_event_t *event)
{
    (void)event;
    notify_activity();
    close_power_detail();
}

static void open_power_detail(void)
{
    if (s_power_detail_overlay != nullptr || s_menu_root == nullptr) {
        return;
    }

    s_power_detail_overlay = lv_obj_create(s_menu_root);
    lv_obj_remove_style_all(s_power_detail_overlay);
    lv_obj_set_size(s_power_detail_overlay, 466, 466);
    lv_obj_center(s_power_detail_overlay);
    lv_obj_add_flag(s_power_detail_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_power_detail_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_power_detail_overlay, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_power_detail_overlay, lv_color_hex(MENU_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_power_detail_overlay, LV_OPA_COVER, 0);

    lv_obj_t *title = make_label(s_power_detail_overlay, "Battery", lv_color_hex(MENU_COLOR_TEXT));
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    s_power_detail_percent =
        make_label(s_power_detail_overlay, "--", lv_color_hex(MENU_COLOR_TEXT));
    lv_obj_set_style_text_font(s_power_detail_percent, &lv_font_montserrat_44, 0);
    lv_obj_align(s_power_detail_percent, LV_ALIGN_TOP_MID, 0, 72);

    s_power_detail_voltage =
        make_label(s_power_detail_overlay, "", lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(s_power_detail_voltage, &lv_font_montserrat_20, 0);
    lv_obj_align(s_power_detail_voltage, LV_ALIGN_TOP_MID, 0, 128);

    s_power_detail_state =
        make_label(s_power_detail_overlay, "Battery", lv_color_hex(MENU_COLOR_TEXT));
    lv_obj_set_style_text_font(s_power_detail_state, &lv_font_montserrat_18, 0);
    lv_obj_align(s_power_detail_state, LV_ALIGN_TOP_MID, 0, 162);

    s_power_detail_extra =
        make_label(s_power_detail_overlay, "", lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(s_power_detail_extra, &lv_font_montserrat_14, 0);
    lv_obj_align(s_power_detail_extra, LV_ALIGN_TOP_MID, 0, 192);

    s_power_detail_config =
        make_label(s_power_detail_overlay, "", lv_color_hex(MENU_COLOR_MUTED));
    lv_obj_set_style_text_font(s_power_detail_config, &lv_font_montserrat_14, 0);
    lv_obj_align(s_power_detail_config, LV_ALIGN_TOP_MID, 0, 214);

    lv_obj_t *close_button = make_action_button(s_power_detail_overlay, "Close", 140);
    lv_obj_align(close_button, LV_ALIGN_TOP_MID, 0, 320);
    lv_obj_add_event_cb(close_button, on_power_detail_close, LV_EVENT_CLICKED, nullptr);

    update_power_detail_labels();
    lv_obj_move_foreground(s_power_detail_overlay);
    notify_activity();
}

static void on_power_card_clicked(lv_event_t *event)
{
    (void)event;
    notify_activity();
    open_power_detail();
}

void clock_settings_menu_update_power_status(const clock_settings_menu_power_status_t *status)
{
    if (status == nullptr) {
        return;
    }

    s_power_status = *status;
    s_power_status_valid = true;

    char primary[32] = {};
    char detail[48] = {};
    bool low_battery = false;

    if (!status->initialized) {
        snprintf(primary, sizeof(primary), "Battery unavailable");
        snprintf(detail, sizeof(detail), "PMU not detected");
    } else if (!status->battery_present) {
        snprintf(primary, sizeof(primary), "No battery");
        snprintf(detail, sizeof(detail), "%s", status->vbus_present ? "USB powered" : "Battery not detected");
    } else {
        snprintf(primary,
                 sizeof(primary),
                 "Battery %s%u%%",
                 status->battery_percent_estimated ? "~" : "",
                 status->battery_percent);
        const char *state = status->charging ? "Charging"
                            : status->full   ? "Full"
                            : status->discharging ? "Discharging"
                                                  : "Standby";
        snprintf(detail,
                 sizeof(detail),
                 "%u.%02u V - %s",
                 status->battery_mv / 1000,
                 (status->battery_mv % 1000) / 10,
                 state);
        low_battery = status->discharging && status->battery_percent <= 15;
    }

    if (s_power_battery != nullptr) {
        set_label_text_if_changed(s_power_battery, primary);
        lv_obj_set_style_text_color(s_power_battery,
                                    lv_color_hex(low_battery ? MENU_COLOR_WARM
                                                             : MENU_COLOR_TEXT),
                                    0);
    }
    if (s_power_battery_detail != nullptr) {
        set_label_text_if_changed(s_power_battery_detail, detail);
        lv_obj_set_style_text_color(s_power_battery_detail,
                                    lv_color_hex(low_battery ? MENU_COLOR_WARM
                                                             : MENU_COLOR_MUTED),
                                    0);
    }
    update_power_detail_labels();
}

void clock_settings_menu_update_timer_status(bool running,
                                             bool paused,
                                             bool pomodoro,
                                             bool break_phase,
                                             uint32_t remaining_sec)
{
    s_timer_status_running = running;
    s_timer_status_paused = paused;
    s_timer_status_pomodoro = pomodoro;
    s_timer_status_break = break_phase;
    s_timer_status_remaining_sec = remaining_sec;
    update_timer_ui();
}

void clock_settings_menu_update_sleep_status(bool playing, bool usb_powered)
{
    s_sleep_status_playing = playing;
    s_sleep_status_usb = usb_powered;
    update_sleep_ui();
}

bool clock_settings_menu_is_setup_open(void)
{
    return s_faces_overlay != nullptr ||
           s_wifi_setup_overlay != nullptr ||
           s_alarm_list_overlay != nullptr ||
           s_alarm_edit_overlay != nullptr ||
           s_timer_overlay != nullptr ||
           s_sleep_overlay != nullptr ||
           s_audio_test_overlay != nullptr;
}

bool clock_settings_menu_is_power_detail_open(void)
{
    return s_power_detail_overlay != nullptr;
}

void clock_settings_menu_open_power_detail(void)
{
    // Host preview helper: open the battery detail overlay without touch input.
    open_power_detail();
}

void clock_settings_menu_open_alarm_list(void)
{
    open_alarm_list();
}

void clock_settings_menu_open_alarm_editor(uint32_t index)
{
    open_alarm_list();
    open_alarm_editor((uint8_t)index);
}

void clock_settings_menu_open_timer(void)
{
    open_timer_overlay();
}

void clock_settings_menu_open_sleep(void)
{
    open_sleep_overlay();
}

void clock_settings_menu_open_audio_test(void)
{
    open_audio_test_overlay();
}

void clock_settings_menu_close_async(clock_settings_menu_close_done_cb_t done_cb, void *user_ctx)
{
    if (s_menu_root == nullptr) {
        if (done_cb != nullptr) {
            done_cb(user_ctx);
        }
        return;
    }
    if (s_menu_closing) {
        return;
    }

    s_menu_closing = true;
    s_close_done_cb = done_cb;
    s_close_done_ctx = user_ctx;

    lv_anim_delete(s_menu_root, nullptr);
    if (s_menu_content != nullptr) {
        lv_anim_delete(s_menu_content, nullptr);
    }

    if (s_menu_content == nullptr) {
        on_menu_close_animation_done(nullptr);
        return;
    }

    const lv_opa_t start_opa = lv_obj_get_style_opa(s_menu_content, LV_PART_MAIN);
    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, s_menu_content);
    lv_anim_set_exec_cb(&fade, menu_set_opa_cb);
    lv_anim_set_values(&fade, start_opa, LV_OPA_TRANSP);
    lv_anim_set_duration(&fade, 120);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&fade, on_menu_content_fade_done);
    lv_anim_start(&fade);

    lv_anim_t scale;
    lv_anim_init(&scale);
    lv_anim_set_var(&scale, s_menu_content);
    lv_anim_set_exec_cb(&scale, menu_set_scale_cb);
    lv_anim_set_values(&scale,
                       lv_obj_get_style_transform_scale_x(s_menu_content, LV_PART_MAIN),
                       240);
    lv_anim_set_duration(&scale, 120);
    lv_anim_set_path_cb(&scale, lv_anim_path_ease_in_out);
    lv_anim_start(&scale);
}

void clock_settings_menu_delete_immediately(void)
{
    reset_sound_pointers();
    s_menu_closing = false;
    s_close_done_cb = nullptr;
    s_close_done_ctx = nullptr;

    if (s_menu_root != nullptr) {
        lv_anim_delete(s_menu_root, nullptr);
        if (s_menu_content != nullptr) {
            lv_anim_delete(s_menu_content, nullptr);
        }
        lv_obj_delete(s_menu_root);
        s_menu_root = nullptr;
    }

    reset_menu_widget_pointers();
}
