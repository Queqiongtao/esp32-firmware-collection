#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "clock_settings_menu.h"
#include "clock_theme.h"
#include "clock_theme_previews.h"

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    std::exit(1); } } while (0)

namespace {
uint32_t now_ms;
uint32_t tick() { return now_ms; }
void advance(uint32_t ms) { now_ms += ms; lv_timer_handler(); }
unsigned requests;
unsigned polls;
uint32_t selected = 99;
unsigned selections;
clock_settings_menu_scan_state_t scan_state;
esp_err_t scan_error = ESP_OK;
bool empty_scan = false;

esp_err_t start_scan(void *) { ++requests; return ESP_OK; }
clock_settings_menu_scan_state_t poll_scan(clock_settings_menu_wifi_ap_t *aps,
    uint32_t capacity, uint32_t *count, esp_err_t *error, void *)
{
    ++polls;
    *error = scan_error;
    *count = scan_state == CLOCK_SETTINGS_MENU_SCAN_DONE && capacity > 0 && !empty_scan ? 1 : 0;
    if (*count) {
        std::snprintf(aps[0].ssid, sizeof(aps[0].ssid), "Test Network");
        aps[0].rssi = -45;
        aps[0].secure = true;
    }
    return scan_state;
}
void choose(uint32_t index, void *) { selected = index; ++selections; }

lv_obj_t *find_text(lv_obj_t *root, const char *text)
{
    if (lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) {
        return nullptr;
    }
    if (lv_obj_check_type(root, &lv_label_class) &&
        std::strcmp(lv_label_get_text(root), text) == 0) {
        return root;
    }
    for (int32_t i = (int32_t)lv_obj_get_child_count(root) - 1; i >= 0; --i) {
        if (auto *found = find_text(lv_obj_get_child(root, i), text)) {
            return found;
        }
    }
    return nullptr;
}

void click(const char *label)
{
    lv_obj_t *text = find_text(lv_screen_active(), label);
    CHECK(text != nullptr);
    lv_obj_t *button = lv_obj_get_parent(text);
    CHECK(lv_obj_check_type(button, &lv_button_class));
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
}
}

int clock_preview_test(const clock_theme_t *const *themes, size_t count)
{
    lv_tick_set_cb(tick);
    clock_settings_t settings = {};
    settings.brightness_percent = 70;
    settings.sound_volume = 30;
    clock_settings_menu_config_t config = {};
    config.settings = &settings;
    config.themes = g_clock_theme_previews;
    config.theme_count = 8;
    config.theme_cb = choose;
    config.wifi_scan_cb = start_scan;
    config.wifi_scan_poll_cb = poll_scan;
    auto create = [&] {
        CHECK(clock_settings_menu_create(lv_screen_active(), &config) != nullptr);
        advance(500);
    };
    create();
    click("Watch faces");
    CHECK(find_text(lv_screen_active(), "Minimal"));
    CHECK(lv_obj_has_state(lv_obj_get_parent(find_text(lv_screen_active(), "Apply")), LV_STATE_DISABLED));
    click("Apply");
    CHECK(selections == 0);
    click("Minimal");
    CHECK(selected == 99 && settings.theme_index == 0);
    click("Segment");
    CHECK(lv_obj_has_state(lv_obj_get_parent(find_text(lv_screen_active(), "Segment")), LV_STATE_CHECKED));
    CHECK(!lv_obj_has_state(lv_obj_get_parent(find_text(lv_screen_active(), "Apply")), LV_STATE_DISABLED));
    click(LV_SYMBOL_RIGHT);
    CHECK(find_text(lv_screen_active(), "Dot Matrix"));
    click(LV_SYMBOL_LEFT);
    CHECK(lv_obj_has_state(lv_obj_get_parent(find_text(lv_screen_active(), "Segment")), LV_STATE_CHECKED));
    click("Minimal");
    CHECK(lv_obj_has_state(lv_obj_get_parent(find_text(lv_screen_active(), "Apply")), LV_STATE_DISABLED));
    click(LV_SYMBOL_RIGHT);
    click("Dot Matrix");
    CHECK(selected == 99 && selections == 0);
    click("Back");
    click("Watch faces");
    CHECK(lv_obj_has_state(lv_obj_get_parent(find_text(lv_screen_active(), "Minimal")), LV_STATE_CHECKED));
    CHECK(lv_obj_has_state(lv_obj_get_parent(find_text(lv_screen_active(), "Apply")), LV_STATE_DISABLED));
    click("Analog");
    clock_settings_menu_delete_immediately(); // Discard pending selection when the menu is destroyed.
    create();
    click("Watch faces");
    CHECK(lv_obj_has_state(lv_obj_get_parent(find_text(lv_screen_active(), "Minimal")), LV_STATE_CHECKED));
    click(LV_SYMBOL_RIGHT);
    click("Dot Matrix");
    click("Apply");
    CHECK(selected == 7 && selections == 1);
    click("Apply"); // Even a repeated event before the menu closes must be ignored.
    click("Simple");
    CHECK(selected == 7 && selections == 1 && settings.theme_index == 0);
    click("Back");

    clock_settings_menu_open_wifi_setup();
    scan_state = CLOCK_SETTINGS_MENU_SCAN_BUSY;
    clock_settings_menu_start_wifi_scan();
    clock_settings_menu_start_wifi_scan();
    CHECK(requests == 1);
    CHECK(find_text(lv_screen_active(), "Scanning..."));
    CHECK(lv_obj_has_state(lv_obj_get_parent(find_text(lv_screen_active(), "Rescan")), LV_STATE_DISABLED));
    advance(110);
    CHECK(find_text(lv_screen_active(), "Scanning..."));
    click("Back");
    scan_state = CLOCK_SETTINGS_MENU_SCAN_DONE;
    advance(110);
    CHECK(!find_text(lv_screen_active(), "Test Network")); // Completion cannot reopen a page.
    clock_settings_menu_start_wifi_scan();
    CHECK(requests == 2);
    CHECK(find_text(lv_screen_active(), "Test Network"));
    click("Back");
    scan_state = CLOCK_SETTINGS_MENU_SCAN_BUSY;
    clock_settings_menu_start_wifi_scan();
    clock_settings_menu_delete_immediately();
    const unsigned before = polls;
    scan_state = CLOCK_SETTINGS_MENU_SCAN_DONE;
    advance(1000);
    CHECK(polls == before); // Timer is deleted along with the menu.
    CHECK(lv_obj_get_child_count(lv_screen_active()) == 0);

    create();
    clock_settings_menu_open_wifi_setup();
    empty_scan = true;
    scan_state = CLOCK_SETTINGS_MENU_SCAN_DONE;
    clock_settings_menu_start_wifi_scan();
    CHECK(find_text(lv_screen_active(), "No networks found"));
    empty_scan = false;
    scan_state = CLOCK_SETTINGS_MENU_SCAN_ERROR;
    scan_error = ESP_ERR_TIMEOUT;
    clock_settings_menu_start_wifi_scan();
    CHECK(find_text(lv_screen_active(), "Scan timed out. Try again"));
    scan_error = ESP_FAIL;
    click("Rescan");
    CHECK(find_text(lv_screen_active(), "Scan failed. Try again"));
    clock_settings_menu_delete_immediately();

    create();
    clock_settings_menu_set_page(4);
    auto *test_label = find_text(lv_screen_active(), "Audio test");
    CHECK(test_label);
    auto *sound_body = lv_obj_get_parent(lv_obj_get_parent(test_label));
    lv_obj_update_layout(sound_body);
    CHECK(lv_obj_get_scroll_bottom(sound_body) > 0);
    lv_obj_scroll_to_y(sound_body, lv_obj_get_scroll_bottom(sound_body), LV_ANIM_OFF);
    auto *alarm_label = find_text(sound_body, "Alarms");
    CHECK(alarm_label);
    click("Audio test");
    CHECK(find_text(lv_screen_active(), "1 kHz"));
    click("Close");
    clock_settings_menu_open_alarm_editor(0);
    click("M +");
    CHECK(find_text(lv_screen_active(), "00:01"));
    click("M -");
    CHECK(find_text(lv_screen_active(), "00:00"));
    clock_settings_menu_delete_immediately();

    // Exercise the actual theme/overlay ownership and reusable snapshot path.
    lv_draw_buf_t *buffer = nullptr;
    size_t warm_heap = 0;
    size_t steady_heap = 0;
    for (unsigned i = 0; i < 360; ++i) {
        const clock_theme_t *theme = themes[i % count];
        tm local = {};
        local.tm_year = 126; local.tm_mon = 10; local.tm_mday = 8;
        local.tm_hour = i % 24; local.tm_min = i % 60; local.tm_sec = i % 60;
        lv_obj_t *root = theme->build(lv_screen_active());
        CHECK(root);
        if (theme->reset) theme->reset();
        CHECK(theme->update);
        theme->update(&local);
        if (i < count) {
            local.tm_hour = 23; local.tm_min = 59; local.tm_sec = 59;
            theme->update(&local);
            local.tm_hour = 0; local.tm_min = 0; local.tm_sec = 0;
            local.tm_mday = 9; local.tm_wday = 1;
            CHECK(theme->update(&local));
            if (std::strcmp(theme->name, "analog") == 0) {
                CHECK(find_text(root, "09"));
            } else if (std::strcmp(theme->name, "m5_classic") == 0) {
                CHECK(find_text(root, "MON"));
            } else if (std::strcmp(theme->name, "m5_simple") == 0) {
                CHECK(find_text(root, "MON \xE2\x80\xA2 09 NOV"));
            } else if (std::strcmp(theme->name, "m5_number_flow") == 0) {
                CHECK(find_text(root, "2026/11/9 MON"));
            } else {
                CHECK(find_text(root, "MON 09 NOV"));
            }
        }
        if (theme->tick) theme->tick(now_ms + 1000);
        if (theme->set_rotation) theme->set_rotation((i % 4) * 90.0f);
        if (theme->set_overlay_visible) theme->set_overlay_visible(false);
        lv_obj_update_layout(lv_screen_active());
        if (!buffer) buffer = lv_snapshot_create_draw_buf(root, LV_COLOR_FORMAT_RGB565);
        CHECK(buffer);
        CHECK(lv_snapshot_take_to_draw_buf(root, LV_COLOR_FORMAT_RGB565, buffer) == LV_RESULT_OK);
        if (theme->set_overlay_visible) theme->set_overlay_visible(true);
        if (theme->destroy_overlay) theme->destroy_overlay();
        lv_obj_delete(root);
        advance(34);
        CHECK(lv_obj_get_child_count(lv_screen_active()) == 0);
#ifdef __GLIBC__
        if (i == 119) warm_heap = mallinfo2().uordblks;
        if (i == 239) steady_heap = mallinfo2().uordblks;
#endif
    }
#ifdef __GLIBC__
    const size_t end_heap = mallinfo2().uordblks;
    std::printf("Host heap after warmup=%zu, after 240 switches=%zu\n", warm_heap, end_heap);
    CHECK(end_heap <= warm_heap + 4096);
    std::printf("Host heap after first 120 measured switches=%zu\n", steady_heap);
    CHECK(end_heap <= steady_heap);
#endif
    lv_draw_buf_destroy(buffer);
    std::puts("PASS UI: theme selection, async scan navigation/error/deletion, 240 theme switches");
    return 0;
}
