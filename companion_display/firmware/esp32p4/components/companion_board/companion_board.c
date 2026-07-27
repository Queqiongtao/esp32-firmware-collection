#include "companion_board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_lcd_jd9165.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_hosted.h"
#include "esp_hosted_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "companion_board";

/* JC1060P470 pin map and panel timing (from the board reference project). */
#define BOARD_DSI_LANE_NUM 2
#define BOARD_DSI_DPI_CLK_MHZ 54
#define BOARD_DSI_PHY_LDO_CHAN 3
#define BOARD_DSI_PHY_LDO_VOLTAGE_MV 2500
#define BOARD_PANEL_HSYNC_PULSE_WIDTH 40
#define BOARD_PANEL_HSYNC_BACK_PORCH 160
#define BOARD_PANEL_HSYNC_FRONT_PORCH 160
#define BOARD_PANEL_VSYNC_PULSE_WIDTH 10
#define BOARD_PANEL_VSYNC_BACK_PORCH 23
#define BOARD_PANEL_VSYNC_FRONT_PORCH 12
#define BOARD_PIN_BACKLIGHT GPIO_NUM_23
#define BOARD_PIN_LCD_RST GPIO_NUM_27
#define BOARD_PIN_TOUCH_RST GPIO_NUM_22
#define BOARD_PIN_TOUCH_INT GPIO_NUM_21
#define BOARD_BACKLIGHT_LEDC_CH LEDC_CHANNEL_0
#define BOARD_BACKLIGHT_LEDC_TIMER LEDC_TIMER_1
#define BOARD_BACKLIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BOARD_I2C_PORT I2C_NUM_1
#define BOARD_I2C_SDA GPIO_NUM_7
#define BOARD_I2C_SCL GPIO_NUM_8
#define BOARD_I2C_CLK_HZ 400000

#define BOARD_TOUCH_INIT_RETRY_BACKOFF_US (500 * 1000)

static i2c_master_bus_handle_t s_i2c_bus;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static int64_t s_touch_next_retry_us;
static bool s_touch_pressed;
static uint16_t s_touch_last_x;
static uint16_t s_touch_last_y;
static EventGroupHandle_t s_hosted_events;

#define HOSTED_EVENT_UP_BIT (1U << 0)
#define HOSTED_EVENT_FAILURE_BIT (1U << 1)

/* esp_hosted 2.x has no public transport-ready getter; the reference project
 * pins this private symbol to detect an already-up transport that will not
 * emit ESP_HOSTED_EVENT_TRANSPORT_UP again. */
extern uint8_t is_transport_tx_ready(void);

static esp_err_t board_i2c_init(void)
{
    if (s_i2c_bus != NULL) {
        return ESP_OK;
    }
    const i2c_master_bus_config_t config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&config, &s_i2c_bus), TAG, "create shared I2C bus failed");
    return ESP_OK;
}

static esp_err_t board_backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = BOARD_BACKLIGHT_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BOARD_BACKLIGHT_LEDC_TIMER,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = BOARD_PIN_BACKLIGHT,
        .speed_mode = BOARD_BACKLIGHT_LEDC_MODE,
        .channel = BOARD_BACKLIGHT_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BOARD_BACKLIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight PWM timer failed");
    return ledc_channel_config(&channel);
}

esp_err_t board_backlight_set(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    const uint32_t duty = (1023U * (uint32_t)percent) / 100U;
    ESP_RETURN_ON_ERROR(ledc_set_duty(BOARD_BACKLIGHT_LEDC_MODE, BOARD_BACKLIGHT_LEDC_CH, duty),
                        TAG, "set backlight duty failed");
    return ledc_update_duty(BOARD_BACKLIGHT_LEDC_MODE, BOARD_BACKLIGHT_LEDC_CH);
}

esp_err_t board_display_init(void)
{
    if (s_panel != NULL) {
        return ESP_OK;
    }

    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    const esp_ldo_channel_config_t ldo_config = {
        .chan_id = BOARD_DSI_PHY_LDO_CHAN,
        .voltage_mv = BOARD_DSI_PHY_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &ldo_mipi_phy), TAG,
                        "acquire MIPI DPHY LDO failed");
    ESP_RETURN_ON_ERROR(board_backlight_init(), TAG, "configure backlight failed");
    ESP_RETURN_ON_ERROR(board_backlight_set(0), TAG, "backlight off failed");

    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = BOARD_DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = CONFIG_BOARD_JC1060_DSI_LANE_BITRATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus), TAG, "create MIPI DSI bus failed");

    esp_lcd_panel_io_handle_t dbi_io = NULL;
    const esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &dbi_io), TAG,
                        "create MIPI DBI IO failed");

    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = BOARD_DSI_DPI_CLK_MHZ,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 2,
        .video_timing = {
            .h_size = BOARD_DISPLAY_H_RES,
            .v_size = BOARD_DISPLAY_V_RES,
            .hsync_back_porch = BOARD_PANEL_HSYNC_BACK_PORCH,
            .hsync_pulse_width = BOARD_PANEL_HSYNC_PULSE_WIDTH,
            .hsync_front_porch = BOARD_PANEL_HSYNC_FRONT_PORCH,
            .vsync_back_porch = BOARD_PANEL_VSYNC_BACK_PORCH,
            .vsync_pulse_width = BOARD_PANEL_VSYNC_PULSE_WIDTH,
            .vsync_front_porch = BOARD_PANEL_VSYNC_FRONT_PORCH,
        },
    };
    jd9165_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    const esp_lcd_panel_dev_config_t dev_config = {
        .reset_gpio_num = BOARD_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9165(dbi_io, &dev_config, &panel), TAG,
                        "create JD9165 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset LCD panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init LCD panel failed");
    ESP_RETURN_ON_ERROR(board_backlight_set(CONFIG_BOARD_JC1060_BACKLIGHT_PERCENT), TAG,
                        "backlight on failed");

    s_panel = panel;
    ESP_LOGI(TAG, "display ready: JD9165 %dx%d, %d lanes @ %d Mbps",
             BOARD_DISPLAY_H_RES, BOARD_DISPLAY_V_RES, BOARD_DSI_LANE_NUM,
             CONFIG_BOARD_JC1060_DSI_LANE_BITRATE_MBPS);
    return ESP_OK;
}

esp_err_t board_display_panel(esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(ret_panel != NULL, ESP_ERR_INVALID_ARG, TAG, "ret_panel is null");
    ESP_RETURN_ON_FALSE(s_panel != NULL, ESP_ERR_INVALID_STATE, TAG, "display not initialized");
    *ret_panel = s_panel;
    return ESP_OK;
}

static esp_err_t board_touch_init(void)
{
    if (s_touch != NULL) {
        return ESP_OK;
    }
    const int64_t now = esp_timer_get_time();
    if (s_touch_next_retry_us != 0 && now < s_touch_next_retry_us) {
        return ESP_ERR_INVALID_STATE;
    }
    s_touch_next_retry_us = now + BOARD_TOUCH_INIT_RETRY_BACKOFF_US;
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c not ready");

    /* Probe the primary GT911 address first, then the backup. driver_data
     * makes the GT911 driver lock the address through the RST/INT sequence;
     * without it the locked address is non-deterministic. */
    const uint8_t addresses[] = {
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP,
    };
    esp_err_t err = ESP_FAIL;
    for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
        esp_lcd_panel_io_handle_t io = NULL;
        esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        io_config.scl_speed_hz = BOARD_I2C_CLK_HZ;
        io_config.dev_addr = addresses[i];
        esp_lcd_touch_io_gt911_config_t gt911_config = {
            .dev_addr = addresses[i],
        };
        const esp_lcd_touch_config_t touch_config = {
            .x_max = CONFIG_BOARD_JC1060_TOUCH_NATIVE_H_RES,
            .y_max = CONFIG_BOARD_JC1060_TOUCH_NATIVE_V_RES,
            .rst_gpio_num = BOARD_PIN_TOUCH_RST,
            .int_gpio_num = BOARD_PIN_TOUCH_INT,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
            .driver_data = &gt911_config,
        };
        err = esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_config, &io);
        if (err != ESP_OK) {
            continue;
        }
        err = esp_lcd_touch_new_i2c_gt911(io, &touch_config, &s_touch);
        if (err == ESP_OK) {
            s_touch_next_retry_us = 0;
            ESP_LOGI(TAG, "touch ready: GT911 @ 0x%02X (native %dx%d)", addresses[i],
                     CONFIG_BOARD_JC1060_TOUCH_NATIVE_H_RES, CONFIG_BOARD_JC1060_TOUCH_NATIVE_V_RES);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "GT911 init failed at 0x%02X: %s", addresses[i], esp_err_to_name(err));
        esp_lcd_panel_io_del(io);
    }
    return err;
}

static uint16_t board_touch_scale(uint16_t value, uint32_t panel_size, uint32_t native_size)
{
    uint32_t scaled = (uint32_t)value * panel_size / native_size;
    if (scaled >= panel_size) {
        scaled = panel_size - 1;
    }
    return (uint16_t)scaled;
}

esp_err_t board_touch_read(uint16_t *x, uint16_t *y, uint8_t *pressed, uint8_t *point_num)
{
    ESP_RETURN_ON_FALSE(x != NULL && y != NULL && pressed != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "touch output is null");
    ESP_RETURN_ON_ERROR(board_touch_init(), TAG, "touch init failed");

    esp_err_t err = esp_lcd_touch_read_data(s_touch);
    if (err != ESP_OK) {
        s_touch_pressed = false;
        return err;
    }
    esp_lcd_touch_point_data_t point[1] = {};
    uint8_t count = 0;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_get_data(s_touch, point, &count, 1), TAG,
                        "touch get data failed");
    if (count > 0) {
        s_touch_last_x = board_touch_scale(point[0].x, BOARD_DISPLAY_H_RES,
                                           CONFIG_BOARD_JC1060_TOUCH_NATIVE_H_RES);
        s_touch_last_y = board_touch_scale(point[0].y, BOARD_DISPLAY_V_RES,
                                           CONFIG_BOARD_JC1060_TOUCH_NATIVE_V_RES);
        s_touch_pressed = true;
        *x = s_touch_last_x;
        *y = s_touch_last_y;
        *pressed = 1;
        if (point_num != NULL) {
            *point_num = 1;
        }
        return ESP_OK;
    }
    if (s_touch_pressed) {
        /* Release event carries the last stable coordinates. */
        s_touch_pressed = false;
        *x = s_touch_last_x;
        *y = s_touch_last_y;
        *pressed = 0;
        if (point_num != NULL) {
            *point_num = BOARD_TOUCH_RELEASE_EVENT_POINT_NUM;
        }
        return ESP_OK;
    }
    *x = 0;
    *y = 0;
    *pressed = 0;
    if (point_num != NULL) {
        *point_num = 0;
    }
    return ESP_OK;
}

static void board_hosted_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (s_hosted_events == NULL) {
        return;
    }
    if (id == ESP_HOSTED_EVENT_TRANSPORT_UP) {
        xEventGroupSetBits(s_hosted_events, HOSTED_EVENT_UP_BIT);
    } else if (id == ESP_HOSTED_EVENT_TRANSPORT_DOWN || id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE) {
        ESP_LOGW(TAG, "hosted transport event %ld", (long)id);
        xEventGroupSetBits(s_hosted_events, HOSTED_EVENT_FAILURE_BIT);
    }
}

esp_err_t board_network_init(void)
{
    esp_err_t err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG,
                        "create default event loop failed");
    if (s_hosted_events == NULL) {
        s_hosted_events = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_hosted_events != NULL, ESP_ERR_NO_MEM, TAG,
                            "create hosted event group failed");
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID,
                                                                board_hosted_event_handler, NULL, NULL),
                            TAG, "register hosted event failed");
    }

    if (is_transport_tx_ready()) {
        ESP_LOGI(TAG, "ESP-Hosted transport already up");
        return ESP_OK;
    }
    xEventGroupClearBits(s_hosted_events, HOSTED_EVENT_UP_BIT | HOSTED_EVENT_FAILURE_BIT);
    ESP_LOGI(TAG, "connecting ESP-Hosted transport to the C6 co-processor");
    ESP_RETURN_ON_ERROR(esp_hosted_connect_to_slave(), TAG, "esp_hosted_connect_to_slave failed");
    if (is_transport_tx_ready()) {
        xEventGroupSetBits(s_hosted_events, HOSTED_EVENT_UP_BIT);
    }
    const EventBits_t bits = xEventGroupWaitBits(
        s_hosted_events, HOSTED_EVENT_UP_BIT | HOSTED_EVENT_FAILURE_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(CONFIG_BOARD_JC1060_NETWORK_READY_TIMEOUT_MS));
    ESP_RETURN_ON_FALSE((bits & HOSTED_EVENT_FAILURE_BIT) == 0, ESP_FAIL, TAG,
                        "hosted transport failed");
    ESP_RETURN_ON_FALSE((bits & HOSTED_EVENT_UP_BIT) != 0, ESP_ERR_TIMEOUT, TAG,
                        "hosted transport wait timed out");
    ESP_LOGI(TAG, "ESP-Hosted transport up");
    return ESP_OK;
}
