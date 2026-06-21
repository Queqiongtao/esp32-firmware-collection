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
static constexpr uint32_t MENU_PAGE_COUNT = 3;
static constexpr int32_t MENU_PAGE_HEIGHT = 270;
static constexpr int32_t MENU_PAGE_GAP = 14;
static constexpr int32_t MENU_PAGE_STRIDE = MENU_PAGE_HEIGHT + MENU_PAGE_GAP;

static lv_obj_t *s_menu_root = nullptr;
static lv_obj_t *s_brightness_value = nullptr;
static lv_obj_t *s_wifi_status = nullptr;
static lv_obj_t *s_page_dots[MENU_PAGE_COUNT] = {};
static lv_obj_t *s_sleep_buttons[SLEEP_OPTION_COUNT] = {};
static clock_settings_menu_config_t s_config = {};

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    return label;
}

static void style_slider(lv_obj_t *slider)
{
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x252A2E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x7EE7EA), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
}

static void style_switch(lv_obj_t *sw)
{
    const lv_style_selector_t checked_indicator =
        (lv_style_selector_t)LV_PART_INDICATOR | (lv_style_selector_t)LV_STATE_CHECKED;

    lv_obj_set_style_bg_color(sw, lv_color_hex(0x252A2E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x7EE7EA), checked_indicator);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, checked_indicator);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
}

static void style_sleep_button(lv_obj_t *button, bool active)
{
    lv_obj_set_style_bg_color(button, active ? lv_color_hex(0x7EE7EA) : lv_color_hex(0x171B1F), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, 24, 0);
    lv_obj_set_style_border_width(button, 0, 0);
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

static void update_sleep_buttons(uint32_t sleep_timeout_sec)
{
    const size_t active_index = sleep_index_from_value(sleep_timeout_sec);
    for (size_t i = 0; i < SLEEP_OPTION_COUNT; ++i) {
        if (s_sleep_buttons[i] == nullptr) {
            continue;
        }
        style_sleep_button(s_sleep_buttons[i], i == active_index);
    }
}

static void update_wifi_status(bool enabled)
{
    if (s_wifi_status == nullptr) {
        return;
    }
    lv_label_set_text(s_wifi_status, enabled ? "Not set" : "Off");
}

static void update_page_dots(uint32_t active_index)
{
    for (uint32_t i = 0; i < MENU_PAGE_COUNT; ++i) {
        if (s_page_dots[i] == nullptr) {
            continue;
        }
        const bool active = i == active_index;
        lv_obj_set_size(s_page_dots[i], active ? 18 : 6, 6);
        lv_obj_set_style_bg_color(s_page_dots[i],
                                  active ? lv_color_hex(0x7EE7EA) : lv_color_hex(0x3A3F44),
                                  0);
    }
}

static void scroll_to_page(lv_obj_t *wheel, uint32_t active_index, lv_anim_enable_t anim)
{
    if (active_index >= MENU_PAGE_COUNT) {
        active_index = MENU_PAGE_COUNT - 1;
    }
    update_page_dots(active_index);
    lv_obj_scroll_to_y(wheel, (int32_t)active_index * MENU_PAGE_STRIDE, anim);
}

static void on_wheel_scroll_end(lv_event_t *event)
{
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

static void on_sleep_clicked(lv_event_t *event)
{
    notify_activity();
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
    if (s_config.close_cb != nullptr) {
        s_config.close_cb(s_config.user_ctx);
    }
}

static lv_obj_t *make_setting_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, 320, MENU_PAGE_HEIGHT);
    lv_obj_set_style_margin_bottom(page, MENU_PAGE_GAP, 0);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return page;
}

static lv_obj_t *make_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = make_label(parent, text, lv_color_hex(0xF7F9FA));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    return label;
}

static lv_obj_t *make_value_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = make_label(parent, text, lv_color_hex(0xA6A6A6));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
    return label;
}

lv_obj_t *clock_settings_menu_create(lv_obj_t *parent, const clock_settings_menu_config_t *config)
{
    if (parent == nullptr || config == nullptr || config->settings == nullptr) {
        return nullptr;
    }

    s_config = *config;
    s_brightness_value = nullptr;
    s_wifi_status = nullptr;
    for (size_t i = 0; i < MENU_PAGE_COUNT; ++i) {
        s_page_dots[i] = nullptr;
    }
    for (size_t i = 0; i < SLEEP_OPTION_COUNT; ++i) {
        s_sleep_buttons[i] = nullptr;
    }

    s_menu_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_menu_root);
    lv_obj_set_size(s_menu_root, 466, 466);
    lv_obj_center(s_menu_root);
    lv_obj_add_flag(s_menu_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_menu_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_menu_root, lv_color_hex(0x050607), 0);
    lv_obj_set_style_bg_opa(s_menu_root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_menu_root, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(s_menu_root, on_root_pressed, LV_EVENT_PRESSED, nullptr);

    lv_obj_t *content = lv_obj_create(s_menu_root);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, 334, 400);
    lv_obj_center(content);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = make_label(content, "Settings", lv_color_hex(0xF7F9FA));
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_margin_bottom(title, 10, 0);

    lv_obj_t *wheel = lv_obj_create(content);
    lv_obj_remove_style_all(wheel);
    lv_obj_set_size(wheel, 326, MENU_PAGE_HEIGHT);
    lv_obj_set_style_pad_top(wheel, 0, 0);
    lv_obj_set_style_pad_bottom(wheel, 0, 0);
    lv_obj_set_style_pad_left(wheel, 3, 0);
    lv_obj_set_style_pad_right(wheel, 3, 0);
    lv_obj_set_flex_flow(wheel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wheel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(wheel, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(wheel, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(wheel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(wheel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(wheel, on_wheel_scroll_end, LV_EVENT_SCROLL_END, nullptr);

    lv_obj_t *wifi_page = make_setting_page(wheel);
    lv_obj_t *wifi_caption = make_value_label(wifi_page, "NETWORK");
    lv_obj_set_style_text_font(wifi_caption, &lv_font_montserrat_14, 0);
    make_title(wifi_page, "WiFi");
    s_wifi_status = make_value_label(wifi_page, "");
    lv_obj_set_style_margin_bottom(s_wifi_status, 14, 0);
    update_wifi_status(config->settings->wifi_enabled);
    lv_obj_t *wifi_switch = lv_switch_create(wifi_page);
    lv_obj_set_size(wifi_switch, 112, 54);
    lv_obj_add_flag(wifi_switch, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    style_switch(wifi_switch);
    if (config->settings->wifi_enabled) {
        lv_obj_add_state(wifi_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(wifi_switch, on_wifi_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *brightness_page = make_setting_page(wheel);
    lv_obj_t *brightness_caption = make_value_label(brightness_page, "DISPLAY");
    lv_obj_set_style_text_font(brightness_caption, &lv_font_montserrat_14, 0);
    make_title(brightness_page, "Brightness");
    s_brightness_value = make_value_label(brightness_page, "");
    lv_obj_set_style_text_font(s_brightness_value, &lv_font_montserrat_28, 0);
    update_brightness_value(config->settings->brightness_percent);
    lv_obj_t *slider = lv_slider_create(brightness_page);
    lv_obj_set_width(slider, 242);
    lv_obj_set_height(slider, 18);
    lv_obj_set_style_margin_top(slider, 20, 0);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    style_slider(slider);
    lv_slider_set_range(slider, 1, 100);
    lv_slider_set_value(slider, config->settings->brightness_percent, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, on_brightness_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *sleep_page = make_setting_page(wheel);
    lv_obj_t *sleep_caption = make_value_label(sleep_page, "POWER");
    lv_obj_set_style_text_font(sleep_caption, &lv_font_montserrat_14, 0);
    make_title(sleep_page, "Sleep");
    lv_obj_t *sleep_grid = lv_obj_create(sleep_page);
    lv_obj_remove_style_all(sleep_grid);
    lv_obj_set_size(sleep_grid, 256, 112);
    lv_obj_set_style_margin_top(sleep_grid, 18, 0);
    lv_obj_set_flex_flow(sleep_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(sleep_grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (size_t i = 0; i < SLEEP_OPTION_COUNT; ++i) {
        lv_obj_t *button = lv_button_create(sleep_grid);
        lv_obj_set_size(button, 108, 48);
        lv_obj_set_style_margin_all(button, 4, 0);
        lv_obj_add_flag(button, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        lv_obj_add_event_cb(button, on_sleep_clicked, LV_EVENT_CLICKED, (void *)i);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, SLEEP_OPTION_LABELS[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(label);
        s_sleep_buttons[i] = button;
    }
    update_sleep_buttons(config->settings->sleep_timeout_sec);

    scroll_to_page(wheel, 0, LV_ANIM_OFF);

    lv_obj_t *dots = lv_obj_create(content);
    lv_obj_remove_style_all(dots);
    lv_obj_set_size(dots, 54, 10);
    lv_obj_set_style_margin_top(dots, 8, 0);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (size_t i = 0; i < MENU_PAGE_COUNT; ++i) {
        s_page_dots[i] = lv_obj_create(dots);
        lv_obj_remove_style_all(s_page_dots[i]);
        lv_obj_set_style_radius(s_page_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_page_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_margin_left(s_page_dots[i], 3, 0);
        lv_obj_set_style_margin_right(s_page_dots[i], 3, 0);
    }
    update_page_dots(0);

    lv_obj_t *close_button = lv_button_create(content);
    lv_obj_set_size(close_button, 224, 56);
    lv_obj_set_style_margin_top(close_button, 6, 0);
    lv_obj_set_style_radius(close_button, 28, 0);
    lv_obj_set_style_bg_color(close_button, lv_color_hex(0x171B1F), 0);
    lv_obj_set_style_bg_opa(close_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(close_button, 0, 0);
    lv_obj_add_event_cb(close_button, on_close_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *close_label = lv_label_create(close_button);
    lv_label_set_text(close_label, "Done");
    lv_obj_set_style_text_color(close_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(close_label, &lv_font_montserrat_20, 0);
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
