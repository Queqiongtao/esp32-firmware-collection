#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int i2c_port;
    int sda_gpio;
    int scl_gpio;
    int i2c_hz;

    int lcd_cs_gpio;
    int lcd_pclk_gpio;
    int lcd_d0_gpio;
    int lcd_d1_gpio;
    int lcd_d2_gpio;
    int lcd_d3_gpio;
    int lcd_te_gpio;
    int lcd_rst_gpio;
    int lcd_h_res;
    int lcd_v_res;
    int lcd_spi_host;
    int lcd_power_exio_num;
    int lcd_power_active_level;
    int lcd_rst_exio_num;
    int lcd_rst_active_level;
    int display_pmu_prepare_delay_ms;
    int display_power_off_hold_ms;
    int display_power_on_settle_ms;
    int display_panel_reset_settle_ms;
    int display_panel_init_settle_ms;
    int display_disp_on_settle_ms;
    int display_post_init_delay_ms;
    int display_low_brightness_percent;
    int display_default_brightness_percent;
    int display_self_test_delay_ms;
    int display_brightness_settle_delay_ms;
    int display_delay_i2c_scan;

    int touch_int_gpio;
    int touch_rst_gpio;
    int touch_rst_exio_num;
    int touch_rst_active_level;

    int sd_d0_gpio;
    int sd_cmd_gpio;
    int sd_clk_gpio;

    int i2s_mclk_gpio;
    int i2s_bclk_gpio;
    int i2s_lrck_gpio;
    int i2s_dout_gpio;
    int i2s_din_gpio;
    int pa_enable_gpio;

    uint8_t es8311_addr;
    uint8_t tca9554_addr;
    uint8_t ft3168_addr;
    uint8_t pcf85063_addr;
    uint8_t qmi8658_addr;
    uint8_t axp2101_addr;
} board_amoled_1_8_config_t;

const board_amoled_1_8_config_t *board_amoled_1_8_get_config(void);

esp_err_t board_i2c_init(void);
esp_err_t board_i2c_scan(void);

esp_err_t board_display_init(void);
esp_err_t board_display_new(esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io);
esp_err_t board_backlight_set(int percent);
esp_err_t board_touch_new(esp_lcd_touch_handle_t *ret_touch);
esp_err_t board_pmu_init(void);
esp_err_t board_pmu_display_power_on(void);
esp_err_t board_pmu_display_power_off(void);
esp_err_t board_pmu_display_power_cycle(void);
esp_err_t board_pmu_get_status(uint16_t *batt_mv, uint8_t *batt_percent, uint8_t *charging);

esp_err_t board_sdcard_mount(void);
esp_err_t board_sdcard_unmount(void);

esp_err_t board_rtc_init(void);
esp_err_t board_rtc_get_time(uint8_t *hour, uint8_t *min, uint8_t *sec);
esp_err_t board_rtc_set_time(uint8_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t min, uint8_t sec);

esp_err_t board_imu_init(void);
esp_err_t board_imu_read(int16_t *ax, int16_t *ay, int16_t *az, int16_t *gx, int16_t *gy, int16_t *gz);

esp_err_t board_audio_init(void);
esp_err_t board_audio_play_tone(int freq_hz, int duration_ms);
esp_err_t board_audio_mic_read(int16_t *buf, int samples);

#ifdef __cplusplus
}
#endif
