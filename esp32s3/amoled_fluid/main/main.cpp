#include <inttypes.h>
#include <stdio.h>

#include "block_physics.h"
#include "board_amoled_1_8.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "amoled_fluid";
static constexpr int IMU_POLL_INTERVAL_MS = 10; /* 100 Hz */

static struct {
    block_physics_handle_t physics;
} s_imu_ctx;

/* IMU 采样任务：100Hz 读取原始数据并注入方块物理（QMI8658 轴映射由 Kconfig 标定） */
static void imu_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    int16_t ax = 0;
    int16_t ay = 0;
    int16_t az = 0;
    int16_t gx = 0;
    int16_t gy = 0;
    int16_t gz = 0;
    while (1) {
        esp_err_t err = board_imu_read(&ax, &ay, &az, &gx, &gy, &gz);
        if (err == ESP_OK) {
            block_physics_imu_update(s_imu_ctx.physics, ax, ay, az, gx, gy, gz);
        } else {
            ESP_LOGW(TAG, "imu read failed: %s", esp_err_to_name(err));
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(IMU_POLL_INTERVAL_MS));
    }
}

extern "C" void app_main(void)
{
    esp_chip_info_t chip_info{};
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "Waveshare ESP32-S3 Touch AMOLED 1.8 project starting");
    ESP_LOGI(TAG,
             "cores=%d, flash=%" PRIu32 "MB, psram=%s",
             chip_info.cores,
             flash_size / (1024 * 1024),
             (chip_info.features & CHIP_FEATURE_EMB_PSRAM) ? "embedded" : "external or unknown");

    const board_amoled_1_8_config_t *cfg = board_amoled_1_8_get_config();
    ESP_LOGI(TAG, "board cfg: lcd(cs=%d pclk=%d d0-d3=%d/%d/%d/%d te=%d exio_pwr=%d exio_rst=%d %dx%d)",
             cfg->lcd_cs_gpio, cfg->lcd_pclk_gpio,
             cfg->lcd_d0_gpio, cfg->lcd_d1_gpio, cfg->lcd_d2_gpio, cfg->lcd_d3_gpio,
             cfg->lcd_te_gpio, cfg->lcd_power_exio_num, cfg->lcd_rst_exio_num,
             cfg->lcd_h_res, cfg->lcd_v_res);
    ESP_LOGI(TAG, "board cfg: imu(qmi8658=0x%02X)", cfg->qmi8658_addr);

    esp_err_t err = board_display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board init failed: %s", esp_err_to_name(err));
    }

    esp_lcd_panel_handle_t panel = NULL;
    err = board_display_new(&panel, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board display new failed: %s", esp_err_to_name(err));
    }

    if (cfg->display_delay_i2c_scan) {
        err = board_i2c_scan();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2c scan failed: %s", esp_err_to_name(err));
        }
    }

    block_physics_handle_t physics = NULL;
    if (panel != NULL) {
        block_physics_config_t physics_cfg = block_physics_default_config();
        physics_cfg.h_res = cfg->lcd_h_res;
        physics_cfg.v_res = cfg->lcd_v_res;
        err = block_physics_create(&physics_cfg, panel, &physics);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "block physics create failed: %s", esp_err_to_name(err));
        } else {
            err = board_imu_init();
            if (err == ESP_OK) {
                s_imu_ctx.physics = physics;
                if (xTaskCreatePinnedToCore(imu_task, "imu_poll", 3072, NULL, 4, NULL, 0) != pdPASS) {
                    ESP_LOGE(TAG, "imu task create failed");
                } else {
                    ESP_LOGI(TAG, "imu sampling started @ %d Hz", 1000 / IMU_POLL_INTERVAL_MS);
                }
            } else {
                ESP_LOGW(TAG, "imu init failed (%s), physics falls back to default gravity",
                         esp_err_to_name(err));
            }
            err = block_physics_start(physics);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "block physics start failed: %s", esp_err_to_name(err));
            }
        }
    } else {
        ESP_LOGE(TAG, "display unavailable, block physics disabled");
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
