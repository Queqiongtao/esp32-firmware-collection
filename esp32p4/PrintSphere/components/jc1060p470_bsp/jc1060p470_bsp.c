#include "bsp/jc1060p470.h"

#include "board_i2c.h"
#include "display_board.h"
#include "display_lvgl.h"
#include "display_panel.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "jc1060p470_bsp";
static display_panel_t s_panel;
static display_lvgl_t s_lvgl;
static bool s_display_ready;

lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, NULL, TAG, "display config is NULL");
    if (s_display_ready) {
        return s_lvgl.display;
    }

    ESP_LOGI(TAG, "Initialize JC1060P470 display: 1024x600 RGB565");
    bool i2c_acquired = false;
    esp_err_t ret = display_board_init();
    if (ret == ESP_OK) {
        ret = display_panel_init(config->tear_avoid_mode, config->rotation, &s_panel);
    }
    if (ret == ESP_OK) {
        ret = board_i2c_init();
        i2c_acquired = ret == ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = display_lvgl_init(&s_panel, &config->lv_adapter_cfg, config->rotation,
                                config->tear_avoid_mode, &s_lvgl);
    }
    if (ret == ESP_OK) {
        ret = display_board_set_backlight(80);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display initialization failed: %s", esp_err_to_name(ret));
        if (s_lvgl.display != NULL) {
            (void)display_lvgl_deinit(&s_lvgl);
        }
        if (i2c_acquired) {
            (void)board_i2c_deinit();
        }
        (void)display_panel_deinit(&s_panel);
        (void)display_board_deinit();
        return NULL;
    }

    s_display_ready = true;
    return s_lvgl.display;
}

esp_err_t bsp_display_lock(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_display_ready, ESP_ERR_INVALID_STATE, TAG, "display is not ready");
    return display_lvgl_lock((int32_t)timeout_ms);
}

void bsp_display_unlock(void)
{
    if (s_display_ready) {
        display_lvgl_unlock();
    }
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (brightness_percent < 0 || brightness_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    return display_board_set_backlight((uint32_t)brightness_percent);
}

esp_err_t bsp_display_rotation_set(bsp_display_rotation_t rotation)
{
    ESP_RETURN_ON_FALSE(s_display_ready, ESP_ERR_INVALID_STATE, TAG, "display is not ready");
    ESP_RETURN_ON_FALSE(rotation == BSP_DISPLAY_ROTATE_0 || rotation == BSP_DISPLAY_ROTATE_180,
                        ESP_ERR_NOT_SUPPORTED, TAG, "JC1060P470 supports landscape 0/180 only");

    ESP_RETURN_ON_ERROR(display_lvgl_lock(-1), TAG, "lock display failed");
    lv_display_set_rotation(s_lvgl.display,
                            rotation == BSP_DISPLAY_ROTATE_180
                                ? LV_DISPLAY_ROTATION_180
                                : LV_DISPLAY_ROTATION_0);
    display_lvgl_unlock();
    return ESP_OK;
}

esp_err_t bsp_i2c_init(void)
{
    return board_i2c_init();
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    i2c_master_bus_handle_t handle = NULL;
    i2c_port_num_t port = I2C_NUM_1;
    return board_i2c_get_bus(&handle, &port) == ESP_OK ? handle : NULL;
}

bool bsp_touch_is_active(void)
{
    return gpio_get_level(BSP_LCD_TOUCH_INT) == 0;
}
