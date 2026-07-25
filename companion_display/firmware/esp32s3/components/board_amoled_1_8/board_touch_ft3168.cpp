#include "board_touch_ft3168.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "board_touch_ft3168";
static constexpr int64_t TOUCH_INIT_RETRY_BACKOFF_US = 500 * 1000;
static constexpr int TOUCH_DOWN_CONFIRM_SAMPLES = 1;
static constexpr int TOUCH_UP_CONFIRM_SAMPLES = 2;
static constexpr int TOUCH_MOVE_CONFIRM_SAMPLES = 1;
static constexpr int64_t TOUCH_CANDIDATE_HOLD_US = 90 * 1000;
static constexpr int TOUCH_COORD_STABLE_DELTA_PX = 10;
static constexpr int TOUCH_MOVE_CONFIRM_DELTA_PX = 12;
static constexpr int64_t TOUCH_IRQ_WINDOW_US = 90 * 1000;
static constexpr int64_t TOUCH_STALE_RELEASE_TIMEOUT_US = 180 * 1000;
static constexpr int TOUCH_NOISE_JUMP_X_PX = 64;
static constexpr int TOUCH_NOISE_JUMP_Y_PX = 6;
static constexpr uint8_t TOUCH_NOISE_STREAK_LIMIT = 2;
static constexpr uint8_t TOUCH_RELEASE_EVENT_POINT_NUM = UINT8_MAX;

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_touch_dev;
static esp_lcd_panel_io_handle_t s_touch_io;
static esp_lcd_touch_handle_t s_touch;
static board_touch_ft3168_config_t s_cfg;
static bool s_ready;
static bool s_initializing;
static int64_t s_next_retry_us;
static esp_err_t s_last_error = ESP_ERR_INVALID_STATE;
static volatile bool s_irq_pending;
static bool s_pressed_stable;
static bool s_candidate_valid;
static uint8_t s_press_consistency;
static uint8_t s_release_consistency;
static uint16_t s_candidate_x;
static uint16_t s_candidate_y;
static uint16_t s_last_x;
static uint16_t s_last_y;
static int64_t s_candidate_until_us;
static int64_t s_irq_window_until_us;
static bool s_move_candidate_valid;
static uint8_t s_move_consistency;
static uint16_t s_move_candidate_x;
static uint16_t s_move_candidate_y;
static uint8_t s_noise_streak;
static bool s_down_reported;
static int64_t s_last_valid_touch_us;

static void board_touch_ft3168_reset_tracking_state(void);
static void board_touch_ft3168_output_release(uint16_t *x, uint16_t *y, uint8_t *pressed,
                                              uint8_t *point_num, const char *reason);

static bool board_touch_ft3168_is_close_with_delta(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, int delta_px)
{
    const int dx = (int)x1 - (int)x2;
    const int dy = (int)y1 - (int)y2;
    return (dx <= delta_px && dx >= -delta_px &&
            dy <= delta_px && dy >= -delta_px);
}

static bool board_touch_ft3168_is_close(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    return board_touch_ft3168_is_close_with_delta(x1, y1, x2, y2, TOUCH_COORD_STABLE_DELTA_PX);
}

static bool board_touch_ft3168_is_noise_jump(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    const int dx = (int)x1 - (int)x2;
    const int dy = (int)y1 - (int)y2;
    const int abs_dx = dx >= 0 ? dx : -dx;
    const int abs_dy = dy >= 0 ? dy : -dy;
    return abs_dx >= TOUCH_NOISE_JUMP_X_PX && abs_dy <= TOUCH_NOISE_JUMP_Y_PX;
}

static void board_touch_ft3168_interrupt_callback(esp_lcd_touch_handle_t tp)
{
    (void)tp;
    s_irq_pending = true;
}

static void board_touch_ft3168_mark_failure(esp_err_t err, const char *stage)
{
    s_ready = false;
    s_initializing = false;
    board_touch_ft3168_reset_tracking_state();
    s_last_error = err;
    s_next_retry_us = esp_timer_get_time() + TOUCH_INIT_RETRY_BACKOFF_US;
    ESP_LOGW(TAG, "touch %s failed: %s, retry after %lld ms",
             stage, esp_err_to_name(err), (long long)(TOUCH_INIT_RETRY_BACKOFF_US / 1000));
}

static void board_touch_ft3168_reset_tracking_state(void)
{
    s_irq_pending = false;
    s_pressed_stable = false;
    s_candidate_valid = false;
    s_press_consistency = 0;
    s_release_consistency = 0;
    s_candidate_until_us = 0;
    s_irq_window_until_us = 0;
    s_move_candidate_valid = false;
    s_move_consistency = 0;
    s_noise_streak = 0;
    s_down_reported = false;
    s_last_valid_touch_us = 0;
}

static void board_touch_ft3168_output_release(uint16_t *x, uint16_t *y, uint8_t *pressed,
                                              uint8_t *point_num, const char *reason)
{
    const bool had_reported_touch = s_down_reported;
    const uint16_t release_x = s_last_x;
    const uint16_t release_y = s_last_y;

    if (reason != NULL && had_reported_touch) {
        ESP_LOGI(TAG, "touch release: %s", reason);
    }
    board_touch_ft3168_reset_tracking_state();
    *pressed = 0;
    *x = had_reported_touch ? release_x : 0;
    *y = had_reported_touch ? release_y : 0;
    if (point_num != NULL) {
        *point_num = had_reported_touch ? TOUCH_RELEASE_EVENT_POINT_NUM : 0;
    }
}

static esp_err_t board_touch_ft3168_raw_read(uint8_t reg, uint8_t *value)
{
    ESP_RETURN_ON_FALSE(value != NULL, ESP_ERR_INVALID_ARG, TAG, "touch raw read value is null");
    ESP_RETURN_ON_FALSE(s_touch_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "touch raw device is null");
    return i2c_master_transmit_receive(s_touch_dev, &reg, 1, value, 1, 100);
}

static esp_err_t board_touch_ft3168_raw_write(uint8_t reg, uint8_t value)
{
    ESP_RETURN_ON_FALSE(s_touch_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "touch raw device is null");
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_touch_dev, buf, sizeof(buf), 100);
}

static esp_err_t board_touch_ft3168_validate(void)
{
    ESP_LOGI(TAG, "touch validate start: addr=0x%02X", s_cfg.i2c_addr);

    uint8_t firm_id = 0;
    uint8_t state = 0;
    uint8_t points = 0;

    ESP_RETURN_ON_ERROR(board_touch_ft3168_raw_read(0xA6, &firm_id), TAG, "touch validate firm_id read failed");
    ESP_RETURN_ON_ERROR(board_touch_ft3168_raw_read(0xA7, &state), TAG, "touch validate state read failed");
    ESP_RETURN_ON_ERROR(board_touch_ft3168_raw_read(0x02, &points), TAG, "touch validate points read failed");

    ESP_LOGI(TAG, "touch validate ok: addr=0x%02X firm_id=0x%02X state=0x%02X points=0x%02X",
             s_cfg.i2c_addr, firm_id, state, points);
    return ESP_OK;
}

static esp_err_t board_touch_ft3168_configure_runtime(void)
{
    ESP_LOGI(TAG, "touch runtime config start");
    ESP_RETURN_ON_ERROR(board_touch_ft3168_raw_write(0xA5, 0x00), TAG, "touch set active mode failed");
    ESP_RETURN_ON_ERROR(board_touch_ft3168_raw_write(0x87, 0x00), TAG, "touch disable monitor timer failed");
    ESP_RETURN_ON_ERROR(board_touch_ft3168_raw_write(0x88, 12), TAG, "touch set active period failed");
    ESP_RETURN_ON_ERROR(board_touch_ft3168_raw_write(0x89, 0x00), TAG, "touch clear monitor period failed");
    ESP_LOGI(TAG, "touch runtime config ok");
    return ESP_OK;
}

static void board_touch_ft3168_release_driver(void)
{
    if (s_touch != NULL) {
        esp_err_t err = esp_lcd_touch_del(s_touch);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "touch del failed: %s", esp_err_to_name(err));
        }
        s_touch = NULL;
    }
    if (s_touch_io != NULL) {
        esp_err_t err = esp_lcd_panel_io_del(s_touch_io);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "touch io del failed: %s", esp_err_to_name(err));
        }
        s_touch_io = NULL;
    }
    s_ready = false;
    board_touch_ft3168_reset_tracking_state();
}

static esp_err_t board_touch_ft3168_create_driver(void)
{
    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max = s_cfg.x_max;
    tp_cfg.y_max = s_cfg.y_max;
    tp_cfg.rst_gpio_num = GPIO_NUM_NC;
    tp_cfg.int_gpio_num = s_cfg.int_gpio_num;
    tp_cfg.levels.reset = 0;
    tp_cfg.levels.interrupt = 0;
    tp_cfg.flags.swap_xy = 0;
    tp_cfg.flags.mirror_x = 0;
    tp_cfg.flags.mirror_y = 0;
    tp_cfg.interrupt_callback = board_touch_ft3168_interrupt_callback;

    esp_lcd_panel_io_i2c_config_t tp_io_cfg = {};
    tp_io_cfg.dev_addr = s_cfg.i2c_addr;
    tp_io_cfg.scl_speed_hz = 100000;
    tp_io_cfg.control_phase_bytes = 1;
    tp_io_cfg.dc_bit_offset = 0;
    tp_io_cfg.lcd_cmd_bits = 8;
    tp_io_cfg.lcd_param_bits = 8;
    tp_io_cfg.flags.disable_control_phase = 1;

    ESP_LOGI(TAG, "touch create driver start: addr=0x%02X", s_cfg.i2c_addr);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_bus, &tp_io_cfg, &s_touch_io), TAG, "create touch panel io failed");
    esp_err_t err = esp_lcd_touch_new_i2c_ft5x06(s_touch_io, &tp_cfg, &s_touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch driver init failed at 0x%02X: %s", s_cfg.i2c_addr, esp_err_to_name(err));
        board_touch_ft3168_release_driver();
        return err;
    }
    err = board_touch_ft3168_configure_runtime();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch runtime config failed at 0x%02X: %s", s_cfg.i2c_addr, esp_err_to_name(err));
        board_touch_ft3168_release_driver();
        return err;
    }

    s_ready = true;
    s_initializing = false;
    s_last_error = ESP_OK;
    s_next_retry_us = 0;
    ESP_LOGI(TAG, "touch ready: FT3168 @ 0x%02X (via ft5x06 driver)", s_cfg.i2c_addr);
    return ESP_OK;
}

static esp_err_t board_touch_ft3168_recover(void)
{
    ESP_LOGW(TAG, "touch recover start");
    board_touch_ft3168_release_driver();
    if (s_cfg.reset_cb != NULL) {
        ESP_RETURN_ON_ERROR(s_cfg.reset_cb(s_cfg.reset_ctx), TAG, "touch recover reset failed");
    }
    ESP_RETURN_ON_ERROR(board_touch_ft3168_validate(), TAG, "touch recover validate failed");
    ESP_RETURN_ON_ERROR(board_touch_ft3168_create_driver(), TAG, "touch recover create failed");
    ESP_LOGI(TAG, "touch recover ok");
    return ESP_OK;
}

bool board_touch_ft3168_is_ready(void)
{
    return s_ready && s_touch != NULL;
}

bool board_touch_ft3168_should_retry_init(void)
{
    if (s_ready) {
        return false;
    }
    if (s_initializing) {
        return false;
    }
    if (s_next_retry_us == 0) {
        return true;
    }
    return esp_timer_get_time() >= s_next_retry_us;
}

esp_err_t board_touch_ft3168_create(i2c_master_bus_handle_t bus,
                                    const board_touch_ft3168_config_t *config,
                                    esp_lcd_touch_handle_t *ret_touch)
{
    ESP_RETURN_ON_FALSE(ret_touch != NULL, ESP_ERR_INVALID_ARG, TAG, "ret_touch is null");

    if (s_ready) {
        *ret_touch = s_touch;
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_ARG, TAG, "touch bus is null");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "touch config is null");
    if (s_initializing) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!board_touch_ft3168_should_retry_init()) {
        return s_last_error;
    }

    s_bus = bus;
    s_cfg = *config;
    s_initializing = true;

    if (s_touch_dev == NULL) {
        ESP_LOGI(TAG, "touch add raw i2c device start: addr=0x%02X", s_cfg.i2c_addr);
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = s_cfg.i2c_addr;
        dev_cfg.scl_speed_hz = 100000;
        esp_err_t err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_touch_dev);
        if (err != ESP_OK) {
            board_touch_ft3168_mark_failure(err, "add raw i2c device");
            return err;
        }
        ESP_LOGI(TAG, "touch add raw i2c device ok");
    }

    esp_err_t err = board_touch_ft3168_validate();
    if (err != ESP_OK) {
        board_touch_ft3168_mark_failure(err, "validate");
        return err;
    }
    err = board_touch_ft3168_create_driver();
    if (err != ESP_OK) {
        board_touch_ft3168_mark_failure(err, "create driver");
        return err;
    }

    *ret_touch = s_touch;
    return ESP_OK;
}

esp_err_t board_touch_ft3168_read(uint16_t *x, uint16_t *y, uint8_t *pressed, uint8_t *point_num)
{
    ESP_RETURN_ON_FALSE(x != NULL, ESP_ERR_INVALID_ARG, TAG, "x is null");
    ESP_RETURN_ON_FALSE(y != NULL, ESP_ERR_INVALID_ARG, TAG, "y is null");
    ESP_RETURN_ON_FALSE(pressed != NULL, ESP_ERR_INVALID_ARG, TAG, "pressed is null");
    ESP_RETURN_ON_FALSE(s_touch != NULL, ESP_ERR_INVALID_STATE, TAG, "touch handle is null");

    const int64_t now = esp_timer_get_time();
    if (s_irq_pending) {
        s_irq_window_until_us = now + TOUCH_IRQ_WINDOW_US;
    }
    const bool irq_window_active = now <= s_irq_window_until_us;
    const bool should_sample = s_irq_pending || s_pressed_stable || (s_candidate_valid && now <= s_candidate_until_us);
    if (!should_sample) {
        if (s_down_reported && s_last_valid_touch_us > 0 &&
            (now - s_last_valid_touch_us) >= TOUCH_STALE_RELEASE_TIMEOUT_US) {
            board_touch_ft3168_output_release(x, y, pressed, point_num, "timeout");
            return ESP_OK;
        }
        *pressed = 0;
        *x = 0;
        *y = 0;
        if (point_num != NULL) {
            *point_num = 0;
        }
        return ESP_OK;
    }

    esp_err_t err = esp_lcd_touch_read_data(s_touch);
    s_irq_pending = false;
    if (err == ESP_ERR_INVALID_RESPONSE) {
        ESP_LOGW(TAG, "touch read nack, start recovery");
        board_touch_ft3168_output_release(x, y, pressed, point_num, "recover");
        err = board_touch_ft3168_recover();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "touch recover failed: %s", esp_err_to_name(err));
            return err;
        }
        return ESP_OK;
    }
    if (err != ESP_OK) {
        board_touch_ft3168_output_release(x, y, pressed, point_num, "read-error");
        return err;
    }

    esp_lcd_touch_point_data_t point_data[1] = {};
    uint8_t local_point_num = 0;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_get_data(s_touch, point_data, &local_point_num, 1), TAG, "touch get data failed");

    const bool has_touch = local_point_num > 0;
    if (has_touch) {
        const uint16_t raw_x = point_data[0].x;
        const uint16_t raw_y = point_data[0].y;
        s_release_consistency = 0;

        if (!s_pressed_stable) {
            if (!irq_window_active) {
                s_candidate_valid = false;
                s_press_consistency = 0;
                *pressed = 0;
                *x = 0;
                *y = 0;
                if (point_num) {
                    *point_num = 0;
                }
                return ESP_OK;
            }
            if (!s_candidate_valid || !board_touch_ft3168_is_close(raw_x, raw_y, s_candidate_x, s_candidate_y)) {
                s_candidate_valid = true;
                s_candidate_x = raw_x;
                s_candidate_y = raw_y;
                s_press_consistency = 1;
            } else if (s_press_consistency < UINT8_MAX) {
                s_press_consistency++;
            }
            s_candidate_until_us = now + TOUCH_CANDIDATE_HOLD_US;

            if (s_press_consistency < TOUCH_DOWN_CONFIRM_SAMPLES) {
                *pressed = 0;
                *x = 0;
                *y = 0;
                if (point_num) {
                    *point_num = 0;
                }
                ESP_LOGD(TAG, "touch debounce discard: candidate x=%u y=%u count=%u",
                         s_candidate_x, s_candidate_y, s_press_consistency);
                return ESP_OK;
            }

            s_pressed_stable = true;
            s_down_reported = true;
            s_candidate_valid = false;
            s_press_consistency = 0;
            s_move_candidate_valid = false;
            s_move_consistency = 0;
            s_noise_streak = 0;
            s_last_x = raw_x;
            s_last_y = raw_y;
        } else {
            if (!irq_window_active && board_touch_ft3168_is_noise_jump(raw_x, raw_y, s_last_x, s_last_y)) {
                if (s_noise_streak < UINT8_MAX) {
                    s_noise_streak++;
                }
                if (s_noise_streak >= TOUCH_NOISE_STREAK_LIMIT) {
                    s_last_valid_touch_us = now;
                    *pressed = s_down_reported ? 1 : 0;
                    *x = s_last_x;
                    *y = s_last_y;
                    if (point_num) {
                        *point_num = s_down_reported ? 1 : 0;
                    }
                    return ESP_OK;
                }
            } else {
                s_noise_streak = 0;
            }

            if (board_touch_ft3168_is_close_with_delta(raw_x, raw_y, s_last_x, s_last_y, TOUCH_MOVE_CONFIRM_DELTA_PX)) {
                s_move_candidate_valid = false;
                s_move_consistency = 0;
                s_last_x = raw_x;
                s_last_y = raw_y;
            } else {
                if (!s_move_candidate_valid ||
                    !board_touch_ft3168_is_close(raw_x, raw_y, s_move_candidate_x, s_move_candidate_y)) {
                    s_move_candidate_valid = true;
                    s_move_candidate_x = raw_x;
                    s_move_candidate_y = raw_y;
                    s_move_consistency = 1;
                } else if (s_move_consistency < UINT8_MAX) {
                    s_move_consistency++;
                }

                if (s_move_consistency >= TOUCH_MOVE_CONFIRM_SAMPLES) {
                    s_last_x = s_move_candidate_x;
                    s_last_y = s_move_candidate_y;
                    s_move_candidate_valid = false;
                    s_move_consistency = 0;
                }
            }
        }

        s_last_valid_touch_us = now;
        *pressed = s_down_reported ? 1 : 0;
        *x = s_last_x;
        *y = s_last_y;
        if (point_num) {
            *point_num = s_down_reported ? 1 : 0;
        }
    } else {
        if (s_pressed_stable) {
            if (s_release_consistency < UINT8_MAX) {
                s_release_consistency++;
            }
            if (s_release_consistency < TOUCH_UP_CONFIRM_SAMPLES) {
                *pressed = 1;
                *x = s_last_x;
                *y = s_last_y;
                if (point_num) {
                    *point_num = 1;
                }
                return ESP_OK;
            }
        }

        board_touch_ft3168_output_release(x, y, pressed, point_num, "no-point");
    }

    return ESP_OK;
}

esp_err_t board_touch_ft3168_deinit(void)
{
    board_touch_ft3168_release_driver();
    if (s_touch_dev != NULL) {
        esp_err_t err = i2c_master_bus_rm_device(s_touch_dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "touch raw device remove failed: %s", esp_err_to_name(err));
            return err;
        }
        s_touch_dev = NULL;
    }
    s_bus = NULL;
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_initializing = false;
    s_next_retry_us = 0;
    s_last_error = ESP_ERR_INVALID_STATE;
    return ESP_OK;
}
