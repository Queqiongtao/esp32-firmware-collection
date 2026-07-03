#include "board_amoled_1_8.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"

#ifndef CONFIG_BOARD_AMOLED_1_8_BACKLIGHT_INVERT
#define CONFIG_BOARD_AMOLED_1_8_BACKLIGHT_INVERT 0
#endif

static const char *TAG = "board_amoled_1_8";

static const board_amoled_1_8_config_t s_board_cfg = {
    .sda_gpio = CONFIG_BOARD_AMOLED_1_8_I2C_SDA_GPIO,
    .scl_gpio = CONFIG_BOARD_AMOLED_1_8_I2C_SCL_GPIO,
    .i2c_port = CONFIG_BOARD_AMOLED_1_8_I2C_PORT,
    .i2c_hz = CONFIG_BOARD_AMOLED_1_8_I2C_HZ,
    .backlight_gpio = CONFIG_BOARD_AMOLED_1_8_BACKLIGHT_GPIO,
    .backlight_ledc_channel = CONFIG_BOARD_AMOLED_1_8_BACKLIGHT_LEDC_CHANNEL,
    .backlight_ledc_timer = CONFIG_BOARD_AMOLED_1_8_BACKLIGHT_LEDC_TIMER,
    .backlight_ledc_mode = CONFIG_BOARD_AMOLED_1_8_BACKLIGHT_LEDC_MODE,
    .backlight_ledc_freq_hz = CONFIG_BOARD_AMOLED_1_8_BACKLIGHT_FREQ_HZ,
    .backlight_invert = CONFIG_BOARD_AMOLED_1_8_BACKLIGHT_INVERT,
};

static bool s_i2c_ready;
static bool s_backlight_ready;
static i2c_master_bus_handle_t s_i2c_bus;

const board_amoled_1_8_config_t *board_amoled_1_8_get_config(void)
{
    return &s_board_cfg;
}

esp_err_t board_i2c_init(void)
{
    if (s_i2c_ready) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = s_board_cfg.i2c_port,
        .sda_io_num = s_board_cfg.sda_gpio,
        .scl_io_num = s_board_cfg.scl_gpio,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c_bus), TAG, "init i2c bus failed");
    s_i2c_ready = true;
    ESP_LOGI(TAG, "i2c ready: port=%d sda=%d scl=%d hz=%d",
             s_board_cfg.i2c_port,
             s_board_cfg.sda_gpio,
             s_board_cfg.scl_gpio,
             s_board_cfg.i2c_hz);
    return ESP_OK;
}

static esp_err_t board_backlight_init(void)
{
    if (s_backlight_ready) {
        return ESP_OK;
    }

    const ledc_timer_config_t timer_cfg = {
        .speed_mode = s_board_cfg.backlight_ledc_mode,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = s_board_cfg.backlight_ledc_timer,
        .freq_hz = s_board_cfg.backlight_ledc_freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "config backlight timer failed");

    const ledc_channel_config_t channel_cfg = {
        .gpio_num = s_board_cfg.backlight_gpio,
        .speed_mode = s_board_cfg.backlight_ledc_mode,
        .channel = s_board_cfg.backlight_ledc_channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = s_board_cfg.backlight_ledc_timer,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_cfg), TAG, "config backlight channel failed");

    s_backlight_ready = true;
    return ESP_OK;
}

esp_err_t board_backlight_set(int percent)
{
    ESP_RETURN_ON_ERROR(board_backlight_init(), TAG, "init backlight failed");

    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }

    uint32_t duty = (uint32_t)((8191ULL * (uint32_t)percent) / 100ULL);
    if (s_board_cfg.backlight_invert) {
        duty = 8191U - duty;
    }

    ESP_RETURN_ON_ERROR(
        ledc_set_duty(s_board_cfg.backlight_ledc_mode, s_board_cfg.backlight_ledc_channel, duty),
        TAG,
        "set backlight duty failed");
    ESP_RETURN_ON_ERROR(
        ledc_update_duty(s_board_cfg.backlight_ledc_mode, s_board_cfg.backlight_ledc_channel),
        TAG,
        "update backlight duty failed");

    return ESP_OK;
}

esp_err_t board_display_init(void)
{
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "init i2c before display failed");
    ESP_RETURN_ON_ERROR(board_backlight_set(80), TAG, "set default backlight failed");

    ESP_LOGW(TAG,
             "display init is currently a board-local placeholder using only ESP-IDF primitives; "
             "panel/touch specific bring-up will be added after confirming 1.8-inch board pinout and controller init sequence");
    return ESP_OK;
}
