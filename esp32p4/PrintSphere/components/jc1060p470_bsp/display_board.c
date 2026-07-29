/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "display_board.h"

#include "display_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"

static const char *TAG = "display_board";
static esp_ldo_channel_handle_t s_dsi_phy_ldo;
static bool s_initialized;

static esp_err_t display_board_enable_dsi_phy_power(void)
{
    if (s_dsi_phy_ldo != NULL) {
        return ESP_OK;
    }
    const esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = DISPLAY_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = DISPLAY_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };

    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &s_dsi_phy_ldo), TAG, "acquire MIPI DPHY LDO failed");
    ESP_LOGI(TAG, "MIPI DSI PHY powered on");
    return ESP_OK;
}

static esp_err_t display_board_init_backlight(void)
{
#if CONFIG_EXAMPLE_LCD_BACKLIGHT_BYPASS_PWM
    const gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << DISPLAY_PIN_NUM_BK_LIGHT,
    };

    return gpio_config(&bk_gpio_config);
#else
    const ledc_channel_config_t bk_channel = {
        .gpio_num = DISPLAY_PIN_NUM_BK_LIGHT,
        .speed_mode = DISPLAY_LCD_LEDC_MODE,
        .channel = DISPLAY_LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = DISPLAY_LCD_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    const ledc_timer_config_t bk_timer = {
        .speed_mode = DISPLAY_LCD_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = DISPLAY_LCD_LEDC_TIMER,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    ESP_RETURN_ON_ERROR(ledc_timer_config(&bk_timer), TAG, "configure backlight PWM timer failed");
    return ledc_channel_config(&bk_channel);
#endif
}

esp_err_t display_board_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(display_board_enable_dsi_phy_power(), TAG, "enable DSI PHY power failed");
    esp_err_t ret = display_board_init_backlight();
    if (ret == ESP_OK) {
        ret = display_board_set_backlight(0);
    }
    if (ret != ESP_OK) {
        (void)display_board_deinit();
        return ret;
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t display_board_deinit(void)
{
    esp_err_t first_err = ESP_OK;
    if (s_initialized) {
#if CONFIG_EXAMPLE_LCD_BACKLIGHT_BYPASS_PWM
        first_err = gpio_reset_pin(DISPLAY_PIN_NUM_BK_LIGHT);
#else
        first_err = ledc_stop(DISPLAY_LCD_LEDC_MODE, DISPLAY_LCD_LEDC_CH, 0);
#endif
        s_initialized = false;
    }
    if (s_dsi_phy_ldo != NULL) {
        esp_err_t ret = esp_ldo_release_channel(s_dsi_phy_ldo);
        if (first_err == ESP_OK) {
            first_err = ret;
        }
        if (ret == ESP_OK) {
            s_dsi_phy_ldo = NULL;
        }
    }
    return first_err;
}

esp_err_t display_board_set_backlight(uint32_t level)
{
#if CONFIG_EXAMPLE_LCD_BACKLIGHT_BYPASS_PWM
    return gpio_set_level(DISPLAY_PIN_NUM_BK_LIGHT, level > 0 ? 1 : 0);
#else
    if (level > 100) {
        level = 100;
    }

    const uint32_t duty_cycle = (1023 * level) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(DISPLAY_LCD_LEDC_MODE, DISPLAY_LCD_LEDC_CH, duty_cycle), TAG, "set backlight duty failed");
    return ledc_update_duty(DISPLAY_LCD_LEDC_MODE, DISPLAY_LCD_LEDC_CH);
#endif
}
