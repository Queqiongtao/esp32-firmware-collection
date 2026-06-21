#include "clock_settings_menu.h"

#include <inttypes.h>
#include <stdio.h>

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

static lv_obj_t *s_menu_root = nullptr;
static lv_obj_t *s_brightness_value = nullptr;
static lv_obj_t *s_wifi_status = nullptr;
static lv_obj_t *s_sleep_roller = nullptr;
static clock_settings_menu_config_t s_config = {};

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    return label;
}

static void style_section(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x121722), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 16, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x202A38), 0);
    lv_obj_set_style_pad_left(obj, 16, 0);
    lv_obj_set_style_pad_right(obj, 16, 0);
    lv_obj_set_style_pad_top(obj, 10, 0);
    lv_obj_set_style_pad_bottom(obj, 10, 0);
}

static void style_slider(lv_obj_t *slider)
{
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2A3342), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x5A8FBF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xE8EEF6), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
}

static void style_switch(lv_obj_t *sw)
{
    const lv_style_selector_t checked_indicator =
        (lv_style_selector_t)LV_PART_INDICATOR | (lv_style_selector_t)LV_STATE_CHECKED;

    lv_obj_set_style_bg_color(sw, lv_color_hex(0x273141), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x426F94), checked_indicator);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, checked_indicator);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xF2F6FA), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
}

static void notify_activity(void)
{
    if (s_config.activity_cb != nullptr) {
        s_config.activity_cb(s_config.user_ctx);
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

static size_t sleep_index_from_value(uint32_t sleep_timeout_sec)
{
    for (size_t i = 0; i < SLEEP_OPTION_COUNT; ++i) {
        if (SLEEP_OPTIONS_SEC[i] == sleep_timeout_sec) {
            return i;
        }
    }
    return 1;
}

static void update_sleep_roller(uint32_t sleep_timeout_sec)
{
    if (s_sleep_roller == nullptr) {
        return;
    }

    lv_roller_set_selected(s_sleep_roller,
                           (uint16_t)sleep_index_from_value(sleep_timeout_sec),
                           LV_ANIM_OFF);
}

static void update_wifi_status(bool enabled)
{
    if (s_wifi_status == nullptr) {
        return;
    }
    lv_label_set_text(s_wifi_status, enabled ? "On - not configured" : "Off");
}

static void on_root_pressed(lv_event_t *event)
{
    (void)event;
    notify_activity();
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

static void on_wifi_changed(lv_event_t *event)
{
    notify_activity();
    lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    update_wifi_status(enabled);
    if (s_config.wifi_cb != nullptr) {
        s_config.wifi_cb(enabled, s_config.user_ctx);
    }
}

static void on_sleep_changed(lv_event_t *event)
{
    notify_activity();
    lv_obj_t *roller = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const uint16_t index = lv_roller_get_selected(roller);
    if (index >= SLEEP_OPTION_COUNT) {
        return;
    }
    const uint32_t sleep_timeout_sec = SLEEP_OPTIONS_SEC[index];
    if (s_config.sleep_cb != nullptr) {
        s_config.sleep_cb(sleep_timeout_sec, s_config.user_ctx);
    }
}

static void on_close_clicked(lv_event_t *event)
{
    (void)event;
    notify_activity();
    if (s_config.close_cb != nullptr) {
        s_config.close_cb(s_config.user_ctx);
    }
}

static lv_obj_t *make_row(lv_obj_t *parent, int height)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, 320);
    lv_obj_set_height(row, height);
    style_section(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static lv_obj_t *make_column_section(lv_obj_t *parent, int height)
{
    lv_obj_t *section = lv_obj_create(parent);
    lv_obj_remove_style_all(section);
    lv_obj_set_width(section, 320);
    lv_obj_set_height(section, height);
    style_section(section);
    lv_obj_set_flex_flow(section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(section, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    return section;
}

static lv_obj_t *make_plain_row(lv_obj_t *parent, int height)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, 288);
    lv_obj_set_height(row, height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

lv_obj_t *clock_settings_menu_create(lv_obj_t *parent, const clock_settings_menu_config_t *config)
{
    if (parent == nullptr || config == nullptr || config->settings == nullptr) {
        return nullptr;
    }

    s_config = *config;
    s_brightness_value = nullptr;
    s_wifi_status = nullptr;
    s_sleep_roller = nullptr;

    s_menu_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_menu_root);
    lv_obj_set_size(s_menu_root, 466, 466);
    lv_obj_center(s_menu_root);
    lv_obj_add_flag(s_menu_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_menu_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_menu_root, lv_color_hex(0x080B10), 0);
    lv_obj_set_style_bg_opa(s_menu_root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_menu_root, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(s_menu_root, on_root_pressed, LV_EVENT_PRESSED, nullptr);

    lv_obj_t *content = lv_obj_create(s_menu_root);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, 326, 368);
    lv_obj_center(content);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = make_label(content, "Settings", lv_color_hex(0xF8FAFC));
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_margin_top(title, 2, 0);

    lv_obj_t *subtitle = make_label(content, "Clock controls", lv_color_hex(0x6F7B8C));
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_14, 0);
    lv_obj_set_style_margin_bottom(subtitle, 10, 0);

    lv_obj_t *wifi_row = make_row(content, 70);
    lv_obj_t *wifi_text = lv_obj_create(wifi_row);
    lv_obj_remove_style_all(wifi_text);
    lv_obj_set_size(wifi_text, 184, 42);
    lv_obj_set_flex_flow(wifi_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wifi_text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    make_label(wifi_text, "WiFi", lv_color_hex(0xE9EEF5));
    s_wifi_status = make_label(wifi_text, "", lv_color_hex(0x8792A2));
    lv_obj_set_style_text_font(s_wifi_status, &lv_font_montserrat_14, 0);
    update_wifi_status(config->settings->wifi_enabled);
    lv_obj_t *wifi_switch = lv_switch_create(wifi_row);
    style_switch(wifi_switch);
    if (config->settings->wifi_enabled) {
        lv_obj_add_state(wifi_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(wifi_switch, on_wifi_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *brightness_section = make_column_section(content, 82);
    lv_obj_t *brightness_row = make_plain_row(brightness_section, 28);
    make_label(brightness_row, "Brightness", lv_color_hex(0xE9EEF5));
    s_brightness_value = make_label(brightness_row, "", lv_color_hex(0x8792A2));
    lv_obj_set_style_text_font(s_brightness_value, &lv_font_montserrat_18, 0);
    update_brightness_value(config->settings->brightness_percent);

    lv_obj_t *slider = lv_slider_create(brightness_section);
    lv_obj_set_width(slider, 288);
    lv_obj_set_height(slider, 10);
    style_slider(slider);
    lv_slider_set_range(slider, 1, 100);
    lv_slider_set_value(slider, config->settings->brightness_percent, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, on_brightness_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *sleep_row = make_row(content, 96);
    lv_obj_t *sleep_text = lv_obj_create(sleep_row);
    lv_obj_remove_style_all(sleep_text);
    lv_obj_set_size(sleep_text, 154, 48);
    lv_obj_set_flex_flow(sleep_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sleep_text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    make_label(sleep_text, "Auto sleep", lv_color_hex(0xE9EEF5));
    lv_obj_t *sleep_hint = make_label(sleep_text, "After idle", lv_color_hex(0x8792A2));
    lv_obj_set_style_text_font(sleep_hint, &lv_font_montserrat_14, 0);

    s_sleep_roller = lv_roller_create(sleep_row);
    lv_obj_set_width(s_sleep_roller, 102);
    lv_obj_set_height(s_sleep_roller, 72);
    lv_roller_set_options(s_sleep_roller, "1m\n5m\n15m\nOff", LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_sleep_roller, 3);
    lv_obj_set_style_bg_color(s_sleep_roller, lv_color_hex(0x0D121A), 0);
    lv_obj_set_style_bg_opa(s_sleep_roller, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_sleep_roller, 1, 0);
    lv_obj_set_style_border_color(s_sleep_roller, lv_color_hex(0x273241), 0);
    lv_obj_set_style_radius(s_sleep_roller, 12, 0);
    lv_obj_set_style_text_color(s_sleep_roller, lv_color_hex(0x8F9BAD), 0);
    lv_obj_set_style_text_font(s_sleep_roller, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_color(s_sleep_roller, lv_color_hex(0x26384A), LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_sleep_roller, lv_color_hex(0xF8FAFC), LV_PART_SELECTED);
    lv_obj_add_event_cb(s_sleep_roller, on_sleep_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    update_sleep_roller(config->settings->sleep_timeout_sec);

    lv_obj_t *close_button = lv_button_create(content);
    lv_obj_set_size(close_button, 132, 40);
    lv_obj_set_style_margin_top(close_button, 12, 0);
    lv_obj_set_style_radius(close_button, 20, 0);
    lv_obj_set_style_bg_color(close_button, lv_color_hex(0x1A2230), 0);
    lv_obj_set_style_bg_opa(close_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(close_button, 1, 0);
    lv_obj_set_style_border_color(close_button, lv_color_hex(0x2D394A), 0);
    lv_obj_add_event_cb(close_button, on_close_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *close_label = lv_label_create(close_button);
    lv_label_set_text(close_label, "Done");
    lv_obj_set_style_text_color(close_label, lv_color_hex(0xE9EEF5), 0);
    lv_obj_set_style_text_font(close_label, &lv_font_montserrat_18, 0);
    lv_obj_center(close_label);

    lv_obj_move_foreground(s_menu_root);
    return s_menu_root;
}

void clock_settings_menu_move_foreground(void)
{
    if (s_menu_root != nullptr) {
        lv_obj_move_foreground(s_menu_root);
    }
}
