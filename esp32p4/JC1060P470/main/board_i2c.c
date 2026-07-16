/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "board_i2c.h"

#include "board_config.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board_i2c";
static i2c_master_bus_handle_t s_bus;

esp_err_t board_i2c_init(void)
{
    if (s_bus != NULL) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BOARD_SHARED_I2C_PORT,
        .sda_io_num = BOARD_SHARED_I2C_SDA,
        .scl_io_num = BOARD_SHARED_I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&config, &s_bus), TAG,
                        "create shared I2C bus failed");
    ESP_LOGI(TAG, "Shared I2C ready: port=%d SDA=%d SCL=%d",
             BOARD_SHARED_I2C_PORT, BOARD_SHARED_I2C_SDA,
             BOARD_SHARED_I2C_SCL);
    return ESP_OK;
}

esp_err_t board_i2c_get_bus(i2c_master_bus_handle_t *out_handle, i2c_port_num_t *out_port)
{
    ESP_RETURN_ON_FALSE(out_handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "out_handle is NULL");
    ESP_RETURN_ON_FALSE(s_bus != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "shared I2C bus is not initialized");

    *out_handle = s_bus;
    if (out_port != NULL) {
        *out_port = BOARD_SHARED_I2C_PORT;
    }
    return ESP_OK;
}
