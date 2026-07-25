#include "board_amoled_1_8.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_types.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_io_expander.h"
#include "esp_io_expander_tca9554.h"
#include "XPowersLib.h"
#include "board_display_priv.h"
#include "board_touch_ft3168.h"

static const char *TAG = "board_amoled_1_8";

#ifndef CONFIG_BOARD_AMOLED_1_8_DISPLAY_PMU_PREPARE_DELAY_MS
#define CONFIG_BOARD_AMOLED_1_8_DISPLAY_PMU_PREPARE_DELAY_MS 100
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_DISPLAY_POWER_OFF_HOLD_MS
#define CONFIG_BOARD_AMOLED_1_8_DISPLAY_POWER_OFF_HOLD_MS 150
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_DISPLAY_POWER_ON_SETTLE_MS
#define CONFIG_BOARD_AMOLED_1_8_DISPLAY_POWER_ON_SETTLE_MS 350
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_DISPLAY_POST_INIT_DELAY_MS
#define CONFIG_BOARD_AMOLED_1_8_DISPLAY_POST_INIT_DELAY_MS 200
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_LCD_RST_EXIO_NUM
#define CONFIG_BOARD_AMOLED_1_8_LCD_RST_EXIO_NUM 0
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_LCD_POWER_EXIO_NUM
#define CONFIG_BOARD_AMOLED_1_8_LCD_POWER_EXIO_NUM 1
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_LCD_TE_GPIO
#define CONFIG_BOARD_AMOLED_1_8_LCD_TE_GPIO 13
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_LCD_POWER_ACTIVE_LEVEL
#define CONFIG_BOARD_AMOLED_1_8_LCD_POWER_ACTIVE_LEVEL 1
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_LCD_RST_ACTIVE_LEVEL
#define CONFIG_BOARD_AMOLED_1_8_LCD_RST_ACTIVE_LEVEL 0
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_TOUCH_RST_EXIO_NUM
#define CONFIG_BOARD_AMOLED_1_8_TOUCH_RST_EXIO_NUM 2
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_TOUCH_RST_ACTIVE_LEVEL
#define CONFIG_BOARD_AMOLED_1_8_TOUCH_RST_ACTIVE_LEVEL 0
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_DISPLAY_PANEL_RESET_SETTLE_MS
#define CONFIG_BOARD_AMOLED_1_8_DISPLAY_PANEL_RESET_SETTLE_MS 150
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_DISPLAY_PANEL_INIT_SETTLE_MS
#define CONFIG_BOARD_AMOLED_1_8_DISPLAY_PANEL_INIT_SETTLE_MS 200
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_DISPLAY_DISP_ON_SETTLE_MS
#define CONFIG_BOARD_AMOLED_1_8_DISPLAY_DISP_ON_SETTLE_MS 120
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_DISPLAY_LOW_BRIGHTNESS_PERCENT
#define CONFIG_BOARD_AMOLED_1_8_DISPLAY_LOW_BRIGHTNESS_PERCENT 20
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_DISPLAY_DEFAULT_BRIGHTNESS_PERCENT
#define CONFIG_BOARD_AMOLED_1_8_DISPLAY_DEFAULT_BRIGHTNESS_PERCENT 80
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_DISPLAY_SELF_TEST_DELAY_MS
#define CONFIG_BOARD_AMOLED_1_8_DISPLAY_SELF_TEST_DELAY_MS 250
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_DISPLAY_BRIGHTNESS_SETTLE_DELAY_MS
#define CONFIG_BOARD_AMOLED_1_8_DISPLAY_BRIGHTNESS_SETTLE_DELAY_MS 120
#endif

#ifndef CONFIG_BOARD_AMOLED_1_8_DISPLAY_DELAY_I2C_SCAN
#define CONFIG_BOARD_AMOLED_1_8_DISPLAY_DELAY_I2C_SCAN 1
#endif

static constexpr int BOARD_TOUCH_RESET_PULSE_MS = 20;
static constexpr int BOARD_TOUCH_RESET_SETTLE_MS = 120;

static const board_amoled_1_8_config_t s_board_cfg = {
    .i2c_port = CONFIG_BOARD_AMOLED_1_8_I2C_PORT,
    .sda_gpio = CONFIG_BOARD_AMOLED_1_8_I2C_SDA_GPIO,
    .scl_gpio = CONFIG_BOARD_AMOLED_1_8_I2C_SCL_GPIO,
    .i2c_hz = CONFIG_BOARD_AMOLED_1_8_I2C_HZ,

    .lcd_cs_gpio = CONFIG_BOARD_AMOLED_1_8_LCD_CS_GPIO,
    .lcd_pclk_gpio = CONFIG_BOARD_AMOLED_1_8_LCD_PCLK_GPIO,
    .lcd_pclk_hz = CONFIG_BOARD_AMOLED_1_8_LCD_PCLK_HZ,
    .lcd_d0_gpio = CONFIG_BOARD_AMOLED_1_8_LCD_D0_GPIO,
    .lcd_d1_gpio = CONFIG_BOARD_AMOLED_1_8_LCD_D1_GPIO,
    .lcd_d2_gpio = CONFIG_BOARD_AMOLED_1_8_LCD_D2_GPIO,
    .lcd_d3_gpio = CONFIG_BOARD_AMOLED_1_8_LCD_D3_GPIO,
    .lcd_te_gpio = CONFIG_BOARD_AMOLED_1_8_LCD_TE_GPIO,
    .lcd_rst_gpio = CONFIG_BOARD_AMOLED_1_8_LCD_RST_GPIO,
    .lcd_h_res = CONFIG_BOARD_AMOLED_1_8_LCD_H_RES,
    .lcd_v_res = CONFIG_BOARD_AMOLED_1_8_LCD_V_RES,
    .lcd_spi_host = CONFIG_BOARD_AMOLED_1_8_LCD_SPI_HOST,
    .lcd_power_exio_num = CONFIG_BOARD_AMOLED_1_8_LCD_POWER_EXIO_NUM,
    .lcd_power_active_level = CONFIG_BOARD_AMOLED_1_8_LCD_POWER_ACTIVE_LEVEL,
    .lcd_rst_exio_num = CONFIG_BOARD_AMOLED_1_8_LCD_RST_EXIO_NUM,
    .lcd_rst_active_level = CONFIG_BOARD_AMOLED_1_8_LCD_RST_ACTIVE_LEVEL,
    .display_pmu_prepare_delay_ms = CONFIG_BOARD_AMOLED_1_8_DISPLAY_PMU_PREPARE_DELAY_MS,
    .display_power_off_hold_ms = CONFIG_BOARD_AMOLED_1_8_DISPLAY_POWER_OFF_HOLD_MS,
    .display_power_on_settle_ms = CONFIG_BOARD_AMOLED_1_8_DISPLAY_POWER_ON_SETTLE_MS,
    .display_panel_reset_settle_ms = CONFIG_BOARD_AMOLED_1_8_DISPLAY_PANEL_RESET_SETTLE_MS,
    .display_panel_init_settle_ms = CONFIG_BOARD_AMOLED_1_8_DISPLAY_PANEL_INIT_SETTLE_MS,
    .display_disp_on_settle_ms = CONFIG_BOARD_AMOLED_1_8_DISPLAY_DISP_ON_SETTLE_MS,
    .display_post_init_delay_ms = CONFIG_BOARD_AMOLED_1_8_DISPLAY_POST_INIT_DELAY_MS,
    .display_low_brightness_percent = CONFIG_BOARD_AMOLED_1_8_DISPLAY_LOW_BRIGHTNESS_PERCENT,
    .display_default_brightness_percent = CONFIG_BOARD_AMOLED_1_8_DISPLAY_DEFAULT_BRIGHTNESS_PERCENT,
    .display_self_test_delay_ms = CONFIG_BOARD_AMOLED_1_8_DISPLAY_SELF_TEST_DELAY_MS,
    .display_brightness_settle_delay_ms = CONFIG_BOARD_AMOLED_1_8_DISPLAY_BRIGHTNESS_SETTLE_DELAY_MS,
    .display_delay_i2c_scan = CONFIG_BOARD_AMOLED_1_8_DISPLAY_DELAY_I2C_SCAN,

    .touch_int_gpio = CONFIG_BOARD_AMOLED_1_8_TOUCH_INT_GPIO,
    .touch_rst_gpio = CONFIG_BOARD_AMOLED_1_8_TOUCH_RST_GPIO,
    .touch_rst_exio_num = CONFIG_BOARD_AMOLED_1_8_TOUCH_RST_EXIO_NUM,
    .touch_rst_active_level = CONFIG_BOARD_AMOLED_1_8_TOUCH_RST_ACTIVE_LEVEL,

    .sd_d0_gpio = CONFIG_BOARD_AMOLED_1_8_SD_D0_GPIO,
    .sd_cmd_gpio = CONFIG_BOARD_AMOLED_1_8_SD_CMD_GPIO,
    .sd_clk_gpio = CONFIG_BOARD_AMOLED_1_8_SD_CLK_GPIO,

    .i2s_mclk_gpio = CONFIG_BOARD_AMOLED_1_8_I2S_MCLK_GPIO,
    .i2s_bclk_gpio = CONFIG_BOARD_AMOLED_1_8_I2S_BCLK_GPIO,
    .i2s_lrck_gpio = CONFIG_BOARD_AMOLED_1_8_I2S_LRCK_GPIO,
    .i2s_dout_gpio = CONFIG_BOARD_AMOLED_1_8_I2S_DOUT_GPIO,
    .i2s_din_gpio = CONFIG_BOARD_AMOLED_1_8_I2S_DIN_GPIO,
    .pa_enable_gpio = CONFIG_BOARD_AMOLED_1_8_PA_ENABLE_GPIO,

    .es8311_addr = CONFIG_BOARD_AMOLED_1_8_ES8311_ADDR,
    .tca9554_addr = CONFIG_BOARD_AMOLED_1_8_TCA9554_ADDR,
    .ft3168_addr = CONFIG_BOARD_AMOLED_1_8_FT3168_ADDR,
    .pcf85063_addr = CONFIG_BOARD_AMOLED_1_8_PCF85063_ADDR,
    .qmi8658_addr = CONFIG_BOARD_AMOLED_1_8_QMI8658_ADDR,
    .axp2101_addr = CONFIG_BOARD_AMOLED_1_8_AXP2101_ADDR,
};

static bool s_i2c_ready;
static i2c_master_bus_handle_t s_i2c_bus;
static esp_io_expander_handle_t s_io_expander;
static bool s_io_expander_ready;

const board_amoled_1_8_config_t *board_amoled_1_8_get_config(void)
{
    return &s_board_cfg;
}

esp_err_t board_i2c_init(void)
{
    if (s_i2c_ready) {
        return ESP_OK;
    }

    i2c_master_bus_config_t cfg = {};
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.i2c_port = s_board_cfg.i2c_port;
    cfg.sda_io_num = (gpio_num_t)s_board_cfg.sda_gpio;
    cfg.scl_io_num = (gpio_num_t)s_board_cfg.scl_gpio;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c_bus), TAG, "init i2c bus failed");
    s_i2c_ready = true;
    ESP_LOGI(TAG, "i2c ready: port=%d sda=%d scl=%d hz=%d",
             s_board_cfg.i2c_port,
             s_board_cfg.sda_gpio,
             s_board_cfg.scl_gpio,
             s_board_cfg.i2c_hz);
    return ESP_OK;
}

esp_err_t board_i2c_scan(void)
{
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c not ready");

    ESP_LOGI(TAG, "scanning i2c bus %d ...", s_board_cfg.i2c_port);
    int found = 0;
    for (uint8_t addr = 0x01; addr < 0x7F; addr++) {
        esp_err_t err = i2c_master_probe(s_i2c_bus, addr, 100);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  found device at 0x%02X", addr);
            found++;
        }
    }
    ESP_LOGI(TAG, "scan done, %d device(s) found", found);
    return ESP_OK;
}

static bool board_i2c_device_present(uint8_t addr)
{
    if (!s_i2c_ready || s_i2c_bus == NULL) {
        return false;
    }
    return i2c_master_probe(s_i2c_bus, addr, 100) == ESP_OK;
}

static esp_err_t board_io_expander_init(void)
{
    if (s_io_expander_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c not ready");
    if (!board_i2c_device_present(s_board_cfg.tca9554_addr)) {
        ESP_LOGW(TAG, "TCA9554 not found at 0x%02X", s_board_cfg.tca9554_addr);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_tca9554(s_i2c_bus, s_board_cfg.tca9554_addr, &s_io_expander),
                        TAG, "create TCA9554 failed");
    s_io_expander_ready = true;
    ESP_LOGI(TAG, "IO expander ready: TCA9554 @ 0x%02X", s_board_cfg.tca9554_addr);
    return ESP_OK;
}

static void board_log_touch_candidates(void)
{
    static const uint8_t touch_candidates[] = {0x38, 0x14, 0x15, 0x5D};
    for (size_t i = 0; i < sizeof(touch_candidates) / sizeof(touch_candidates[0]); ++i) {
        const uint8_t addr = touch_candidates[i];
        ESP_LOGI(TAG, "touch candidate 0x%02X: %s", addr,
                 board_i2c_device_present(addr) ? "present" : "missing");
    }
}

static esp_err_t board_touch_prepare(void)
{
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c not ready");

    esp_err_t err = board_pmu_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pmu pre-init skipped: %s", esp_err_to_name(err));
    }

    err = board_io_expander_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "io expander pre-init skipped: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "touch reset start");
        ESP_RETURN_ON_ERROR(board_touch_reset_pulse(), TAG, "touch reset pulse failed");
        ESP_LOGI(TAG, "touch reset done");
    }

    board_log_touch_candidates();
    ESP_LOGI(TAG, "touch address fixed to 0x%02X", s_board_cfg.ft3168_addr);
    return ESP_OK;
}

esp_err_t board_backlight_set(int percent)
{
    return board_display_module_set_backlight(percent);
}

esp_err_t board_display_init(void)
{
    return board_display_module_init();
}

esp_err_t board_display_new(esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io)
{
    return board_display_module_get(ret_panel, ret_io);
}

esp_err_t board_touch_new(esp_lcd_touch_handle_t *ret_touch)
{
    ESP_RETURN_ON_FALSE(ret_touch != NULL, ESP_ERR_INVALID_ARG, TAG, "ret_touch is null");

    if (board_touch_ft3168_is_ready()) {
        return board_touch_ft3168_create(s_i2c_bus, NULL, ret_touch);
    }
    if (!board_touch_ft3168_should_retry_init()) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(board_touch_prepare(), TAG, "touch prepare failed");

    const board_touch_ft3168_config_t touch_cfg = {
        .i2c_addr = s_board_cfg.ft3168_addr,
        .x_max = (uint16_t)s_board_cfg.lcd_h_res,
        .y_max = (uint16_t)s_board_cfg.lcd_v_res,
        .int_gpio_num = (gpio_num_t)s_board_cfg.touch_int_gpio,
        .reset_cb = [](void *) -> esp_err_t { return board_touch_reset_pulse(); },
        .reset_ctx = NULL,
    };
    return board_touch_ft3168_create(s_i2c_bus, &touch_cfg, ret_touch);
}

esp_err_t board_touch_read(uint16_t *x, uint16_t *y, uint8_t *pressed, uint8_t *point_num)
{
    ESP_RETURN_ON_FALSE(x != NULL, ESP_ERR_INVALID_ARG, TAG, "x is null");
    ESP_RETURN_ON_FALSE(y != NULL, ESP_ERR_INVALID_ARG, TAG, "y is null");
    ESP_RETURN_ON_FALSE(pressed != NULL, ESP_ERR_INVALID_ARG, TAG, "pressed is null");

    esp_lcd_touch_handle_t touch = NULL;
    ESP_RETURN_ON_ERROR(board_touch_new(&touch), TAG, "touch init failed");
    ESP_RETURN_ON_FALSE(touch != NULL, ESP_ERR_INVALID_STATE, TAG, "touch handle is null");
    return board_touch_ft3168_read(x, y, pressed, point_num);
}

static XPowersAXP2101 s_pmu;
static bool s_pmu_ready;

static esp_io_expander_pin_num_t board_exio_num_to_mask(int exio_num)
{
    switch (exio_num) {
    case 0: return IO_EXPANDER_PIN_NUM_0;
    case 1: return IO_EXPANDER_PIN_NUM_1;
    case 2: return IO_EXPANDER_PIN_NUM_2;
    case 3: return IO_EXPANDER_PIN_NUM_3;
    case 4: return IO_EXPANDER_PIN_NUM_4;
    case 5: return IO_EXPANDER_PIN_NUM_5;
    case 6: return IO_EXPANDER_PIN_NUM_6;
    case 7: return IO_EXPANDER_PIN_NUM_7;
    default: return (esp_io_expander_pin_num_t)0;
    }
}

static esp_err_t board_exio_set_level(int exio_num, int level)
{
    ESP_RETURN_ON_FALSE(exio_num >= 0, ESP_ERR_NOT_SUPPORTED, TAG, "exio unavailable");
    ESP_RETURN_ON_ERROR(board_io_expander_init(), TAG, "io expander not ready");

    const esp_io_expander_pin_num_t pin_mask = board_exio_num_to_mask(exio_num);
    ESP_RETURN_ON_FALSE(pin_mask != 0, ESP_ERR_INVALID_ARG, TAG, "invalid exio num");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_io_expander, pin_mask, IO_EXPANDER_OUTPUT), TAG, "set exio dir failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_io_expander, pin_mask, (uint8_t)(level ? 1 : 0)), TAG,
                        "set exio level failed");
    return ESP_OK;
}

esp_err_t board_lcd_power_set(bool enable)
{
    const int active = s_board_cfg.lcd_power_active_level ? 1 : 0;
    const int inactive = active ? 0 : 1;
    const int level = enable ? active : inactive;

    ESP_LOGI(TAG, "LCD power via EXIO%d -> %d", s_board_cfg.lcd_power_exio_num, level);
    return board_exio_set_level(s_board_cfg.lcd_power_exio_num, level);
}

static esp_err_t board_lcd_reset_set_level(int level)
{
    return board_exio_set_level(s_board_cfg.lcd_rst_exio_num, level);
}

esp_err_t board_lcd_reset_pulse(void)
{
    const int active = s_board_cfg.lcd_rst_active_level ? 1 : 0;
    const int inactive = active ? 0 : 1;

    ESP_LOGI(TAG, "LCD reset via EXIO%d assert", s_board_cfg.lcd_rst_exio_num);
    ESP_RETURN_ON_ERROR(board_lcd_reset_set_level(active), TAG, "lcd rst assert failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "LCD reset via EXIO%d deassert", s_board_cfg.lcd_rst_exio_num);
    ESP_RETURN_ON_ERROR(board_lcd_reset_set_level(inactive), TAG, "lcd rst deassert failed");
    vTaskDelay(pdMS_TO_TICKS(s_board_cfg.display_panel_reset_settle_ms));
    ESP_LOGI(TAG, "LCD reset pulse done");
    return ESP_OK;
}

static esp_err_t board_touch_reset_set_level(int level)
{
    return board_exio_set_level(s_board_cfg.touch_rst_exio_num, level);
}

esp_err_t board_touch_reset_pulse(void)
{
    const int active = s_board_cfg.touch_rst_active_level ? 1 : 0;
    const int inactive = active ? 0 : 1;

    ESP_LOGI(TAG, "Touch reset via EXIO%d assert", s_board_cfg.touch_rst_exio_num);
    ESP_RETURN_ON_ERROR(board_touch_reset_set_level(active), TAG, "touch rst assert failed");
    vTaskDelay(pdMS_TO_TICKS(BOARD_TOUCH_RESET_PULSE_MS));
    ESP_LOGI(TAG, "Touch reset via EXIO%d deassert", s_board_cfg.touch_rst_exio_num);
    ESP_RETURN_ON_ERROR(board_touch_reset_set_level(inactive), TAG, "touch rst deassert failed");
    vTaskDelay(pdMS_TO_TICKS(BOARD_TOUCH_RESET_SETTLE_MS));
    ESP_LOGI(TAG, "Touch reset pulse done");
    return ESP_OK;
}

esp_err_t board_pmu_display_power_on(void)
{
    if (!s_pmu.setBLDO1Voltage(3300)) {
        ESP_LOGE(TAG, "PMU set BLDO1 voltage failed");
        return ESP_FAIL;
    }
    if (!s_pmu.enableBLDO1()) {
        ESP_LOGE(TAG, "PMU enable BLDO1 failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "PMU display rail power-on: BLDO1=%umV enabled=%d",
             s_pmu.getBLDO1Voltage(), s_pmu.isEnableBLDO1() ? 1 : 0);
    return ESP_OK;
}

esp_err_t board_pmu_display_power_off(void)
{
    if (!s_pmu.disableBLDO1()) {
        ESP_LOGE(TAG, "PMU disable BLDO1 failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "PMU display rail power-off: BLDO1 enabled=%d", s_pmu.isEnableBLDO1() ? 1 : 0);
    return ESP_OK;
}

esp_err_t board_pmu_display_power_cycle(void)
{
    ESP_RETURN_ON_FALSE(s_pmu_ready, ESP_ERR_INVALID_STATE, TAG, "pmu not ready");

    ESP_RETURN_ON_ERROR(board_pmu_display_power_off(), TAG, "display rail off failed");
    vTaskDelay(pdMS_TO_TICKS(s_board_cfg.display_power_off_hold_ms));
    ESP_RETURN_ON_ERROR(board_pmu_display_power_on(), TAG, "display rail on failed");
    vTaskDelay(pdMS_TO_TICKS(s_board_cfg.display_power_on_settle_ms));
    ESP_LOGI(TAG, "display power cycled");
    return ESP_OK;
}

esp_err_t board_pmu_init(void)
{
    if (s_pmu_ready) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c not ready");
    if (!s_pmu.begin(s_i2c_bus, s_board_cfg.axp2101_addr)) {
        ESP_LOGE(TAG, "AXP2101 begin failed @ 0x%02X", s_board_cfg.axp2101_addr);
        return ESP_FAIL;
    }
    s_pmu_ready = true;
    ESP_RETURN_ON_ERROR(board_pmu_display_power_on(), TAG, "enable display power failed");
    ESP_LOGI(TAG, "PMU ready: AXP2101 @ 0x%02X", s_board_cfg.axp2101_addr);
    return ESP_OK;
}

esp_err_t board_pmu_get_status(uint16_t *batt_mv, uint8_t *batt_percent, uint8_t *charging)
{
    if (!s_pmu_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t mv = s_pmu.getBattVoltage();
    if (batt_mv) *batt_mv = mv;
    if (charging) *charging = s_pmu.isCharging() ? 1 : 0;
    if (batt_percent) {
        if (mv >= 4200) *batt_percent = 100;
        else if (mv <= 3000) *batt_percent = 0;
        else *batt_percent = (uint8_t)(((uint32_t)(mv - 3000)) * 100 / 1200);
    }
    return ESP_OK;
}

/* ========================= SD card (SDMMC 1-bit) ========================= */

static sdmmc_card_t *s_sdcard;
static bool s_sdcard_mounted;

esp_err_t board_sdcard_mount(void)
{
    if (s_sdcard_mounted) {
        return ESP_OK;
    }

    sdmmc_host_t host = {};
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    host.io_voltage = 3.3f;
    host.init = &sdmmc_host_init;
    host.set_bus_width = &sdmmc_host_set_bus_width;
    host.get_bus_width = &sdmmc_host_get_slot_width;
    host.set_card_clk = &sdmmc_host_set_card_clk;
    host.do_transaction = &sdmmc_host_do_transaction;
    host.deinit_p = &sdmmc_host_deinit_slot;

    sdmmc_slot_config_t slot = {};
    slot.clk = (gpio_num_t)s_board_cfg.sd_clk_gpio;
    slot.cmd = (gpio_num_t)s_board_cfg.sd_cmd_gpio;
    slot.d0 = (gpio_num_t)s_board_cfg.sd_d0_gpio;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.cd = GPIO_NUM_NC;
    slot.wp = GPIO_NUM_NC;
    slot.width = 1;
    slot.flags = 0;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 5;
    mount_cfg.allocation_unit_size = 16 * 1024;

    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount_cfg, &s_sdcard);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdcard mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_sdcard_mounted = true;
    uint64_t capacity_mb = ((uint64_t)s_sdcard->csd.capacity * s_sdcard->csd.sector_size) / (1024 * 1024);
    ESP_LOGI(TAG, "sdcard mounted: %s, %lluMB",
             s_sdcard->cid.name, (unsigned long long)capacity_mb);
    return ESP_OK;
}

esp_err_t board_sdcard_unmount(void)
{
    if (!s_sdcard_mounted) {
        return ESP_OK;
    }
    esp_err_t err = esp_vfs_fat_sdcard_unmount("/sdcard", s_sdcard);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdcard unmount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_sdcard = NULL;
    s_sdcard_mounted = false;
    ESP_LOGI(TAG, "sdcard unmounted");
    return ESP_OK;
}

/* ============================ RTC PCF85063 ============================ */

static i2c_master_dev_handle_t s_rtc_dev;
static bool s_rtc_ready;

static inline uint8_t bcd2bin(uint8_t v) { return (uint8_t)(((v >> 4) & 0x0F) * 10 + (v & 0x0F)); }
static inline uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static esp_err_t pcf85063_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_rtc_dev, &reg, 1, buf, len, 100);
}

static esp_err_t pcf85063_write(uint8_t reg, uint8_t val)
{
    uint8_t w[2] = {reg, val};
    return i2c_master_transmit(s_rtc_dev, w, sizeof(w), 100);
}

esp_err_t board_rtc_init(void)
{
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c not ready");

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = s_board_cfg.pcf85063_addr;
    dev_cfg.scl_speed_hz = (uint32_t)s_board_cfg.i2c_hz;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_rtc_dev),
                        TAG, "add rtc device failed");

    uint8_t sec = 0;
    esp_err_t err = pcf85063_read(0x04, &sec, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCF85063 probe failed @ 0x%02X: %s", s_board_cfg.pcf85063_addr, esp_err_to_name(err));
        return err;
    }
    s_rtc_ready = true;
    ESP_LOGI(TAG, "RTC ready: PCF85063 @ 0x%02X", s_board_cfg.pcf85063_addr);
    return ESP_OK;
}

esp_err_t board_rtc_get_time(uint8_t *hour, uint8_t *min, uint8_t *sec)
{
    if (!s_rtc_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[7] = {0};
    esp_err_t err = pcf85063_read(0x04, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    if (sec) *sec = bcd2bin(buf[0] & 0x7F);
    if (min) *min = bcd2bin(buf[1] & 0x7F);
    if (hour) *hour = bcd2bin(buf[2] & 0x3F);
    return ESP_OK;
}

esp_err_t board_rtc_set_time(uint8_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t min, uint8_t sec)
{
    if (!s_rtc_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(pcf85063_write(0x04, bin2bcd(sec)), TAG, "rtc set sec");
    ESP_RETURN_ON_ERROR(pcf85063_write(0x05, bin2bcd(min)), TAG, "rtc set min");
    ESP_RETURN_ON_ERROR(pcf85063_write(0x06, bin2bcd(hour)), TAG, "rtc set hour");
    ESP_RETURN_ON_ERROR(pcf85063_write(0x07, bin2bcd(day)), TAG, "rtc set day");
    ESP_RETURN_ON_ERROR(pcf85063_write(0x08, 0x01), TAG, "rtc set weekday");
    ESP_RETURN_ON_ERROR(pcf85063_write(0x09, bin2bcd(month)), TAG, "rtc set month");
    ESP_RETURN_ON_ERROR(pcf85063_write(0x0A, bin2bcd(year)), TAG, "rtc set year");
    return ESP_OK;
}

/* ============================ IMU QMI8658 ============================ */

static i2c_master_dev_handle_t s_imu_dev;
static bool s_imu_ready;

static esp_err_t qmi8658_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_imu_dev, &reg, 1, buf, len, 100);
}

static esp_err_t qmi8658_write(uint8_t reg, uint8_t val)
{
    uint8_t w[2] = {reg, val};
    return i2c_master_transmit(s_imu_dev, w, sizeof(w), 100);
}

esp_err_t board_imu_init(void)
{
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c not ready");

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = s_board_cfg.qmi8658_addr;
    dev_cfg.scl_speed_hz = (uint32_t)s_board_cfg.i2c_hz;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_imu_dev),
                        TAG, "add imu device failed");

    uint8_t whoami = 0;
    ESP_RETURN_ON_ERROR(qmi8658_read(0x00, &whoami, 1), TAG, "imu whoami read");
    if (whoami != 0x05) {
        ESP_LOGE(TAG, "QMI8658 whoami mismatch: 0x%02X (expect 0x05) @ 0x%02X",
                 whoami, s_board_cfg.qmi8658_addr);
        return ESP_ERR_NOT_FOUND;
    }

    /* CTRL1: address auto-increment for multi-byte reads (bit6) */
    ESP_RETURN_ON_ERROR(qmi8658_write(0x02, 0x40), TAG, "imu ctrl1");
    /* CTRL2: accel ±2g (scale=0) | ODR 128Hz = 0x0C */
    ESP_RETURN_ON_ERROR(qmi8658_write(0x03, 0x0C), TAG, "imu ctrl2");
    /* CTRL3: gyro ±2000dps (0x70) | ODR 128Hz = 0x0C => 0x7C */
    ESP_RETURN_ON_ERROR(qmi8658_write(0x04, 0x7C), TAG, "imu ctrl3");
    /* CTRL7: enable accel (bit0) + gyro (bit1) */
    ESP_RETURN_ON_ERROR(qmi8658_write(0x08, 0x03), TAG, "imu ctrl7");

    s_imu_ready = true;
    ESP_LOGI(TAG, "IMU ready: QMI8658 @ 0x%02X (whoami=0x%02X)", s_board_cfg.qmi8658_addr, whoami);
    return ESP_OK;
}

esp_err_t board_imu_read(int16_t *ax, int16_t *ay, int16_t *az, int16_t *gx, int16_t *gy, int16_t *gz)
{
    if (!s_imu_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[12] = {0};
    esp_err_t err = qmi8658_read(0x35, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    if (ax) *ax = (int16_t)((buf[1] << 8) | buf[0]);
    if (ay) *ay = (int16_t)((buf[3] << 8) | buf[2]);
    if (az) *az = (int16_t)((buf[5] << 8) | buf[4]);
    if (gx) *gx = (int16_t)((buf[7] << 8) | buf[6]);
    if (gy) *gy = (int16_t)((buf[9] << 8) | buf[8]);
    if (gz) *gz = (int16_t)((buf[11] << 8) | buf[10]);
    return ESP_OK;
}

/* ============================ Audio ES8311 ============================ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BOARD_AUDIO_SAMPLE_RATE  16000
#define BOARD_AUDIO_CHANNELS     2
#define BOARD_AUDIO_BITS         16
#define BOARD_AUDIO_TONE_AMP     12000  /* int16 amplitude, leave headroom */

static i2s_chan_handle_t s_i2s_tx;
static i2s_chan_handle_t s_i2s_rx;
static const audio_codec_data_if_t  *s_codec_data_if;
static const audio_codec_ctrl_if_t  *s_codec_ctrl_if;
static const audio_codec_gpio_if_t  *s_codec_gpio_if;
static const audio_codec_if_t       *s_codec_if;
static esp_codec_dev_handle_t        s_codec_dev;
static bool s_audio_ready;

esp_err_t board_audio_init(void)
{
    if (s_audio_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c not ready for audio");

    /* ---- I2S STD channels (TX + RX) ---- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx),
                        TAG, "i2s new channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)s_board_cfg.i2s_mclk_gpio,
            .bclk = (gpio_num_t)s_board_cfg.i2s_bclk_gpio,
            .ws = (gpio_num_t)s_board_cfg.i2s_lrck_gpio,
            .dout = (gpio_num_t)s_board_cfg.i2s_dout_gpio,
            .din = (gpio_num_t)s_board_cfg.i2s_din_gpio,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg),
                        TAG, "i2s tx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_rx, &std_cfg),
                        TAG, "i2s rx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "i2s tx enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx), TAG, "i2s rx enable failed");

    /* ---- codec data interface wired to I2S ---- */
    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = I2S_NUM_0;
    i2s_cfg.tx_handle = s_i2s_tx;
    i2s_cfg.rx_handle = s_i2s_rx;
    s_codec_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_codec_data_if != NULL, ESP_FAIL, TAG, "codec data if new failed");

    /* ---- codec I2C control interface (esp_codec_dev expects 8-bit addr) ---- */
    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port = (uint8_t)s_board_cfg.i2c_port;
    i2c_cfg.addr = (uint8_t)(s_board_cfg.es8311_addr << 1);
    i2c_cfg.bus_handle = s_i2c_bus;
    s_codec_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_codec_ctrl_if != NULL, ESP_FAIL, TAG, "codec ctrl if new failed");

    /* ---- codec GPIO interface (drives PA pin via es8311 driver) ---- */
    s_codec_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_codec_gpio_if != NULL, ESP_FAIL, TAG, "codec gpio if new failed");

    /* ---- ES8311 codec interface, ADC+DAC both enabled ---- */
    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.codec_mode  = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8311_cfg.ctrl_if    = s_codec_ctrl_if;
    es8311_cfg.gpio_if    = s_codec_gpio_if;
    es8311_cfg.pa_pin     = (int16_t)s_board_cfg.pa_enable_gpio;
    es8311_cfg.pa_reverted= false;
    es8311_cfg.master_mode= false;
    es8311_cfg.use_mclk   = true;
    s_codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(s_codec_if != NULL, ESP_FAIL, TAG, "es8311 codec new failed");

    /* ---- codec device wrapping codec + data interfaces ---- */
    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT;
    dev_cfg.codec_if = s_codec_if;
    dev_cfg.data_if  = s_codec_data_if;
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec_dev != NULL, ESP_FAIL, TAG, "codec dev new failed");

    int ret = esp_codec_dev_set_out_vol(s_codec_dev, 70);
    ESP_LOGI(TAG, "audio set_out_vol 70: %d", ret);
    ret = esp_codec_dev_set_out_mute(s_codec_dev, false);
    ESP_LOGI(TAG, "audio set_out_mute off: %d", ret);
    ret = esp_codec_dev_set_in_gain(s_codec_dev, 30.0f);
    ESP_LOGI(TAG, "audio set_in_gain 30: %d", ret);

    s_audio_ready = true;
    ESP_LOGI(TAG, "Audio ready: ES8311 @ 0x%02X, I2S(%d/%d/%d/%d/%d) PA=%d",
             s_board_cfg.es8311_addr,
             s_board_cfg.i2s_mclk_gpio, s_board_cfg.i2s_bclk_gpio,
             s_board_cfg.i2s_lrck_gpio, s_board_cfg.i2s_dout_gpio,
             s_board_cfg.i2s_din_gpio, s_board_cfg.pa_enable_gpio);
    return ESP_OK;
}

esp_err_t board_audio_play_tone(int freq_hz, int duration_ms)
{
    if (!s_audio_ready || s_codec_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (freq_hz <= 0 || duration_ms <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = BOARD_AUDIO_BITS;
    fs.channel         = BOARD_AUDIO_CHANNELS;
    fs.sample_rate     = BOARD_AUDIO_SAMPLE_RATE;
    int ret = esp_codec_dev_open(s_codec_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open failed: %d", ret);
        return ESP_FAIL;
    }

    const int total_frames = (int)((uint64_t)duration_ms * BOARD_AUDIO_SAMPLE_RATE / 1000);
    const int chunk_frames = 512;
    int16_t *pcm = (int16_t *)malloc((size_t)chunk_frames * BOARD_AUDIO_CHANNELS * sizeof(int16_t));
    ESP_RETURN_ON_FALSE(pcm != NULL, ESP_ERR_NO_MEM, TAG, "tone pcm alloc failed");

    const double step = 2.0 * M_PI * freq_hz / BOARD_AUDIO_SAMPLE_RATE;
    double phase = 0.0;
    int frames_left = total_frames;
    while (frames_left > 0) {
        int n = frames_left < chunk_frames ? frames_left : chunk_frames;
        for (int i = 0; i < n; i++) {
            int16_t s = (int16_t)(BOARD_AUDIO_TONE_AMP * sin(phase));
            phase += step;
            if (phase >= 2.0 * M_PI) {
                phase -= 2.0 * M_PI;
            }
            pcm[i * BOARD_AUDIO_CHANNELS + 0] = s;
            pcm[i * BOARD_AUDIO_CHANNELS + 1] = s;
        }
        int bytes = n * BOARD_AUDIO_CHANNELS * (BOARD_AUDIO_BITS / 8);
        int wret = esp_codec_dev_write(s_codec_dev, pcm, bytes);
        if (wret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codec write failed: %d", wret);
            free(pcm);
            esp_codec_dev_close(s_codec_dev);
            return ESP_FAIL;
        }
        frames_left -= n;
    }
    free(pcm);

    esp_codec_dev_close(s_codec_dev);
    ESP_LOGI(TAG, "audio play tone: %dHz %dms done", freq_hz, duration_ms);
    return ESP_OK;
}

esp_err_t board_audio_mic_read(int16_t *buf, int samples)
{
    if (!s_audio_ready || s_codec_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (buf == NULL || samples <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = BOARD_AUDIO_BITS;
    fs.channel         = BOARD_AUDIO_CHANNELS;
    fs.sample_rate     = BOARD_AUDIO_SAMPLE_RATE;
    int ret = esp_codec_dev_open(s_codec_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open for mic failed: %d", ret);
        return ESP_FAIL;
    }

    int bytes = samples * (int)sizeof(int16_t);
    ret = esp_codec_dev_read(s_codec_dev, buf, bytes);
    esp_codec_dev_close(s_codec_dev);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec read failed: %d", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}
