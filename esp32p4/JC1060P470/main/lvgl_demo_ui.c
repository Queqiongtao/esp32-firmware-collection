/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "lvgl.h"
#include "esp_log.h"
#include "test_audio.h"

static lv_style_t style_bullet;
static lv_obj_t *scale1;
static lv_obj_t *label_revenue;
static lv_obj_t *label_sales;
static lv_obj_t *label_costs;
static lv_obj_t *s_audio_record_btn;
static lv_obj_t *s_audio_play_btn;
static lv_obj_t *s_audio_status_label;
static lv_obj_t *s_audio_size_label;
static lv_obj_t *s_audio_peak_label;
static lv_obj_t *s_audio_input_label;
static lv_obj_t *s_audio_record_label;
static lv_obj_t *s_audio_play_label;
static test_audio_state_t s_audio_last_state = TEST_AUDIO_STATE_UNINITIALIZED;
static size_t s_audio_last_record_size = SIZE_MAX;
static uint16_t s_audio_last_record_peak = UINT16_MAX;
static size_t s_audio_last_nonzero_samples = SIZE_MAX;
static esp_err_t s_audio_last_error = ESP_OK;
static const lv_font_t *font_normal = &lv_font_montserrat_16;

static lv_obj_t *create_scale_box(lv_obj_t *parent, const char *text1, const char *text2, const char *text3)
{
    lv_obj_t *scale = lv_scale_create(parent);
    lv_obj_center(scale);
    lv_obj_set_size(scale, 600, 600);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_OUTER);
    lv_scale_set_label_show(scale, false);
    lv_scale_set_post_draw(scale, true);
    lv_obj_set_width(scale, LV_PCT(100));
    lv_obj_set_style_pad_all(scale, 30, 0);

    lv_obj_t *bullet1 = lv_obj_create(parent);
    lv_obj_set_size(bullet1, 13, 13);
    lv_obj_remove_style(bullet1, NULL, LV_PART_SCROLLBAR);
    lv_obj_add_style(bullet1, &style_bullet, 0);
    lv_obj_set_style_bg_color(bullet1, lv_palette_main(LV_PALETTE_RED), 0);
    label_revenue = lv_label_create(parent);
    lv_label_set_text(label_revenue, text1);

    lv_obj_t *bullet2 = lv_obj_create(parent);
    lv_obj_set_size(bullet2, 13, 13);
    lv_obj_remove_style(bullet2, NULL, LV_PART_SCROLLBAR);
    lv_obj_add_style(bullet2, &style_bullet, 0);
    lv_obj_set_style_bg_color(bullet2, lv_palette_main(LV_PALETTE_BLUE), 0);
    label_sales = lv_label_create(parent);
    lv_label_set_text(label_sales, text2);

    lv_obj_t *bullet3 = lv_obj_create(parent);
    lv_obj_set_size(bullet3, 13, 13);
    lv_obj_remove_style(bullet3,  NULL, LV_PART_SCROLLBAR);
    lv_obj_add_style(bullet3, &style_bullet, 0);
    lv_obj_set_style_bg_color(bullet3, lv_palette_main(LV_PALETTE_GREEN), 0);
    label_costs = lv_label_create(parent);
    lv_label_set_text(label_costs, text3);

    static int32_t grid_col_dsc[] = {LV_GRID_CONTENT, LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t grid_row_dsc[] = {LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(parent, grid_col_dsc, grid_row_dsc);
    lv_obj_set_grid_cell(scale, LV_GRID_ALIGN_START, 0, 2, LV_GRID_ALIGN_START, 1, 1);
    lv_obj_set_grid_cell(bullet1, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_START, 2, 1);
    lv_obj_set_grid_cell(bullet2, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_START, 3, 1);
    lv_obj_set_grid_cell(bullet3, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_START, 4, 1);
    lv_obj_set_grid_cell(label_revenue, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_START, 2, 1);
    lv_obj_set_grid_cell(label_sales, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_START, 3, 1);
    lv_obj_set_grid_cell(label_costs, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_START, 4, 1);
    return scale;
}

static void scale1_indic1_anim_cb(void *var, int32_t v)
{
    lv_arc_set_value(var, v);
    lv_label_set_text_fmt(label_revenue, "Revenue: %"LV_PRId32" %%", v);
}

static void scale1_indic2_anim_cb(void *var, int32_t v)
{
    lv_arc_set_value(var, v);
    lv_label_set_text_fmt(label_sales, "Sales: %"LV_PRId32" %%", v);
}

static void scale1_indic3_anim_cb(void *var, int32_t v)
{
    lv_arc_set_value(var, v);
    lv_label_set_text_fmt(label_costs, "Costs: %"LV_PRId32" %%", v);
}

static const char *audio_state_to_text(test_audio_state_t state)
{
    switch (state) {
    case TEST_AUDIO_STATE_UNINITIALIZED:
        return "Audio: uninitialized";
    case TEST_AUDIO_STATE_IDLE:
        return "Audio: idle";
    case TEST_AUDIO_STATE_RECORDING:
        return "Audio: recording";
    case TEST_AUDIO_STATE_RECORDED:
        return "Audio: recorded";
    case TEST_AUDIO_STATE_PLAYING:
        return "Audio: playing";
    case TEST_AUDIO_STATE_ERROR:
        return "Audio: error";
    default:
        return "Audio: unknown";
    }
}

static const char *audio_input_level_text(uint16_t peak, size_t nonzero_samples)
{
    if (peak == 0 || nonzero_samples == 0) {
        return "Input: silent";
    }
    if (peak < 256) {
        return "Input: weak";
    }
    return "Input: active";
}

static void audio_ui_update(void)
{
    test_audio_state_t state = test_audio_get_state();
    size_t record_size = test_audio_get_last_record_size();
    uint16_t record_peak = test_audio_get_last_record_peak();
    size_t nonzero_samples = test_audio_get_last_record_nonzero_count();
    esp_err_t last_error = test_audio_get_last_error();
    bool can_record = false;
    bool can_play = false;

    if (!s_audio_status_label || !s_audio_size_label || !s_audio_peak_label || !s_audio_input_label ||
        !s_audio_record_btn || !s_audio_play_btn) {
        return;
    }

    can_record = (state == TEST_AUDIO_STATE_IDLE || state == TEST_AUDIO_STATE_RECORDED || state == TEST_AUDIO_STATE_ERROR);
    can_play = (state == TEST_AUDIO_STATE_RECORDED && record_size > 0);

    if (state == s_audio_last_state && record_size == s_audio_last_record_size &&
        record_peak == s_audio_last_record_peak && nonzero_samples == s_audio_last_nonzero_samples &&
        last_error == s_audio_last_error) {
        return;
    }

    if (state == TEST_AUDIO_STATE_ERROR) {
        lv_label_set_text_fmt(s_audio_status_label, "Audio: error (0x%x)", (unsigned int)last_error);
    } else {
        lv_label_set_text(s_audio_status_label, audio_state_to_text(state));
    }
    lv_label_set_text_fmt(s_audio_size_label, "Recorded: %u bytes", (unsigned int)record_size);
    lv_label_set_text_fmt(s_audio_peak_label, "Peak: %u", (unsigned int)record_peak);
    lv_label_set_text(s_audio_input_label, audio_input_level_text(record_peak, nonzero_samples));

    if (can_record) {
        lv_obj_clear_state(s_audio_record_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_audio_record_btn, LV_STATE_DISABLED);
    }

    if (can_play) {
        lv_obj_clear_state(s_audio_play_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_audio_play_btn, LV_STATE_DISABLED);
    }

    s_audio_last_state = state;
    s_audio_last_record_size = record_size;
    s_audio_last_record_peak = record_peak;
    s_audio_last_nonzero_samples = nonzero_samples;
    s_audio_last_error = last_error;
}

static void audio_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    audio_ui_update();
}

static void audio_record_btn_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = test_audio_start_record();
    if (ret != ESP_OK) {
        ESP_LOGE("AUDIO_UI", "start record failed: %s", esp_err_to_name(ret));
    }
    audio_ui_update();
}

static void audio_play_btn_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = test_audio_start_playback();
    if (ret != ESP_OK) {
        ESP_LOGE("AUDIO_UI", "start playback failed: %s", esp_err_to_name(ret));
    }
    audio_ui_update();
}

void example_lvgl_demo_ui(lv_display_t *disp)
{
    // init default theme
    lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED), LV_THEME_DEFAULT_DARK,
                          font_normal);
    // bullet style
    lv_style_init(&style_bullet);
    lv_style_set_border_width(&style_bullet, 0);
    lv_style_set_radius(&style_bullet, LV_RADIUS_CIRCLE);

    lv_obj_t *parent = lv_display_get_screen_active(disp);

    // create scale widget
    scale1 = create_scale_box(parent, "Revenue", "Sales", "Costs");

    // create arc indicators
    lv_obj_t *arc;
    arc = lv_arc_create(scale1);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_style(arc, NULL, LV_PART_MAIN);
    lv_obj_set_size(arc, lv_pct(100), lv_pct(100));
    lv_obj_set_style_arc_opa(arc, 0, 0);
    lv_obj_set_style_arc_width(arc, 15, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_palette_main(LV_PALETTE_BLUE), LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);

    // animation
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_values(&a, 20, 100);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, scale1_indic1_anim_cb);
    lv_anim_set_var(&a, arc);
    lv_anim_set_duration(&a, 4100);
    lv_anim_set_playback_duration(&a, 2700);
    lv_anim_start(&a);

    arc = lv_arc_create(scale1);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_size(arc, lv_pct(100), lv_pct(100));
    lv_obj_set_style_margin_all(arc, 20, 0);
    lv_obj_set_style_arc_opa(arc, 0, 0);
    lv_obj_set_style_arc_width(arc, 15, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_palette_main(LV_PALETTE_RED), LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(arc);

    lv_anim_set_exec_cb(&a, scale1_indic2_anim_cb);
    lv_anim_set_var(&a, arc);
    lv_anim_set_duration(&a, 2600);
    lv_anim_set_playback_duration(&a, 3200);
    lv_anim_start(&a);

    arc = lv_arc_create(scale1);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_size(arc, lv_pct(100), lv_pct(100));
    lv_obj_set_style_margin_all(arc, 40, 0);
    lv_obj_set_style_arc_opa(arc, 0, 0);
    lv_obj_set_style_arc_width(arc, 15, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_palette_main(LV_PALETTE_GREEN), LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(arc);

    lv_anim_set_exec_cb(&a, scale1_indic3_anim_cb);
    lv_anim_set_var(&a, arc);
    lv_anim_set_duration(&a, 2800);
    lv_anim_set_playback_duration(&a, 1800);
    lv_anim_start(&a);

    lv_obj_t *audio_panel = lv_obj_create(parent);
    lv_obj_set_size(audio_panel, 420, 210);
    lv_obj_align(audio_panel, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_pad_all(audio_panel, 16, 0);
    lv_obj_set_style_radius(audio_panel, 8, 0);

    s_audio_status_label = lv_label_create(audio_panel);
    lv_obj_align(s_audio_status_label, LV_ALIGN_TOP_LEFT, 0, 0);

    s_audio_size_label = lv_label_create(audio_panel);
    lv_obj_align(s_audio_size_label, LV_ALIGN_TOP_LEFT, 0, 28);

    s_audio_peak_label = lv_label_create(audio_panel);
    lv_obj_align(s_audio_peak_label, LV_ALIGN_TOP_LEFT, 0, 56);

    s_audio_input_label = lv_label_create(audio_panel);
    lv_obj_align(s_audio_input_label, LV_ALIGN_TOP_LEFT, 0, 84);

    s_audio_record_btn = lv_button_create(audio_panel);
    lv_obj_set_size(s_audio_record_btn, 170, 52);
    lv_obj_align(s_audio_record_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(s_audio_record_btn, audio_record_btn_cb, LV_EVENT_CLICKED, NULL);

    s_audio_record_label = lv_label_create(s_audio_record_btn);
    lv_label_set_text(s_audio_record_label, "Start Record");
    lv_obj_center(s_audio_record_label);

    s_audio_play_btn = lv_button_create(audio_panel);
    lv_obj_set_size(s_audio_play_btn, 170, 52);
    lv_obj_align(s_audio_play_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(s_audio_play_btn, audio_play_btn_cb, LV_EVENT_CLICKED, NULL);

    s_audio_play_label = lv_label_create(s_audio_play_btn);
    lv_label_set_text(s_audio_play_label, "Play Recording");
    lv_obj_center(s_audio_play_label);

    audio_ui_update();
    lv_timer_create(audio_ui_timer_cb, 500, NULL);
}
