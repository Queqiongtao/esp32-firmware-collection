#include "board_display_priv.h"

#include "board_amoled_1_8.h"

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_display";

static bool s_display_ready;
static bool s_spi_bus_ready;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;

static const sh8601_lcd_init_cmd_t s_sh8601_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

static void board_display_delay_ms(int delay_ms)
{
    if (delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static esp_err_t board_display_prepare_power(const board_amoled_1_8_config_t *cfg)
{
    ESP_RETURN_ON_ERROR(board_lcd_power_set(true), TAG, "lcd power enable failed");
    if (cfg->lcd_te_gpio >= 0) {
        ESP_LOGI(TAG, "LCD TE reserved on GPIO%d", cfg->lcd_te_gpio);
    }

    esp_err_t err = board_pmu_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display PMU prepare skipped: %s", esp_err_to_name(err));
        board_display_delay_ms(cfg->display_pmu_prepare_delay_ms);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "display PMU ready");
    ESP_LOGI(TAG, "display PMU prepare delay start: %dms", cfg->display_pmu_prepare_delay_ms);
    board_display_delay_ms(cfg->display_pmu_prepare_delay_ms);
    ESP_LOGI(TAG, "display PMU prepare delay end");
    ESP_RETURN_ON_ERROR(board_pmu_display_power_cycle(), TAG, "display power cycle failed");
    ESP_RETURN_ON_ERROR(board_lcd_reset_pulse(), TAG, "lcd reset pulse failed");
    return ESP_OK;
}

static esp_err_t board_display_create_panel(const board_amoled_1_8_config_t *cfg)
{
    if (!s_spi_bus_ready) {
        ESP_LOGI(TAG, "display spi bus init start");
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = cfg->lcd_pclk_gpio;
        buscfg.data0_io_num = cfg->lcd_d0_gpio;
        buscfg.data1_io_num = cfg->lcd_d1_gpio;
        buscfg.data2_io_num = cfg->lcd_d2_gpio;
        buscfg.data3_io_num = cfg->lcd_d3_gpio;
        buscfg.data4_io_num = -1;
        buscfg.data5_io_num = -1;
        buscfg.data6_io_num = -1;
        buscfg.data7_io_num = -1;
        buscfg.data_io_default_level = false;
        buscfg.max_transfer_sz = cfg->lcd_h_res * cfg->lcd_v_res * 16 / 8;
        buscfg.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_SCLK | SPICOMMON_BUSFLAG_QUAD;
        buscfg.isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO;
        buscfg.intr_flags = 0;
        ESP_RETURN_ON_ERROR(spi_bus_initialize((spi_host_device_t)cfg->lcd_spi_host, &buscfg, SPI_DMA_CH_AUTO),
                            TAG, "init spi bus failed");
        s_spi_bus_ready = true;
        ESP_LOGI(TAG, "display spi bus init done");
    }

    ESP_LOGI(TAG, "display panel io create start");
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = (gpio_num_t)cfg->lcd_cs_gpio;
    io_config.dc_gpio_num = GPIO_NUM_NC;
    io_config.spi_mode = 0;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.on_color_trans_done = NULL;
    io_config.user_ctx = NULL;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.cs_ena_pretrans = 0;
    io_config.cs_ena_posttrans = 0;
    io_config.flags.quad_mode = 1;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)cfg->lcd_spi_host, &io_config, &s_panel_io),
                        TAG, "create panel io failed");
    ESP_LOGI(TAG, "display panel io create done");

    ESP_LOGI(TAG, "display panel create start");
    sh8601_vendor_config_t vendor_config = {
        .init_cmds = s_sh8601_init_cmds,
        .init_cmds_size = sizeof(s_sh8601_init_cmds) / sizeof(s_sh8601_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
    panel_config.bits_per_pixel = 16;
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.vendor_config = &vendor_config;
    panel_config.flags.reset_active_high = 0;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8601(s_panel_io, &panel_config, &s_panel), TAG, "create sh8601 failed");
    ESP_LOGI(TAG, "display panel create done");
    return ESP_OK;
}

static esp_err_t board_display_set_brightness_local(int percent)
{
    ESP_RETURN_ON_FALSE(s_panel_io != NULL, ESP_ERR_INVALID_STATE, TAG, "panel io is null");
    ESP_RETURN_ON_FALSE(percent >= 0 && percent <= 100, ESP_ERR_INVALID_ARG, TAG, "invalid brightness");

    uint8_t brightness = (uint8_t)(percent * 255 / 100);
    uint32_t lcd_cmd = 0x51;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= 0x02 << 24;
    return esp_lcd_panel_io_tx_param(s_panel_io, lcd_cmd, &brightness, 1);
}

esp_err_t board_display_module_get(esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io)
{
    const board_amoled_1_8_config_t *cfg = board_amoled_1_8_get_config();

    if (s_display_ready) {
        if (ret_panel) {
            *ret_panel = s_panel;
        }
        if (ret_io) {
            *ret_io = s_panel_io;
        }
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c init failed");
    ESP_RETURN_ON_ERROR(board_display_prepare_power(cfg), TAG, "display power prepare failed");
    ESP_RETURN_ON_ERROR(board_display_create_panel(cfg), TAG, "display create panel failed");

    ESP_LOGI(TAG, "display panel reset start");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_LOGI(TAG, "display panel reset done");
    board_display_delay_ms(cfg->display_panel_reset_settle_ms);

    ESP_LOGI(TAG, "display panel init start");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_LOGI(TAG, "display panel init done");
    board_display_delay_ms(cfg->display_panel_init_settle_ms);

    ESP_LOGI(TAG, "display disp_on start");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel disp_on failed");
    ESP_LOGI(TAG, "display disp_on done");
    board_display_delay_ms(cfg->display_disp_on_settle_ms);

    ESP_RETURN_ON_ERROR(board_display_set_brightness_local(cfg->display_low_brightness_percent), TAG,
                        "set low brightness failed");
    ESP_LOGI(TAG, "display brightness settle start: %dms", cfg->display_brightness_settle_delay_ms);
    board_display_delay_ms(cfg->display_brightness_settle_delay_ms);
    ESP_LOGI(TAG, "display brightness settle end");
    ESP_LOGI(TAG, "display backlight staged to %d%%", cfg->display_low_brightness_percent);

    s_display_ready = true;
    if (ret_panel) {
        *ret_panel = s_panel;
    }
    if (ret_io) {
        *ret_io = s_panel_io;
    }
    ESP_LOGI(TAG, "display ready via local SH8601 path: %dx%d", cfg->lcd_h_res, cfg->lcd_v_res);
    return ESP_OK;
}

esp_err_t board_display_module_set_backlight(int percent)
{
    if (!s_display_ready || s_panel_io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(board_display_set_brightness_local(percent), TAG, "set backlight failed");
    ESP_LOGI(TAG, "backlight %d%%", percent);
    return ESP_OK;
}

esp_err_t board_display_module_init(void)
{
    const board_amoled_1_8_config_t *cfg = board_amoled_1_8_get_config();

    ESP_RETURN_ON_ERROR(board_display_module_get(NULL, NULL), TAG, "display new failed");
    ESP_RETURN_ON_ERROR(board_display_module_set_backlight(cfg->display_default_brightness_percent), TAG,
                        "default backlight failed");
    board_display_delay_ms(cfg->display_brightness_settle_delay_ms);
    ESP_LOGI(TAG, "display self-test armed");
    return ESP_OK;
}
