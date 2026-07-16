/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "test_sdcard.h"

#include "test_config.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>

static const char *TAG = "test_sdcard";

static sdmmc_card_t *s_card = NULL;
static sd_pwr_ctrl_handle_t s_pwr_ctrl_handle = NULL;
static bool s_mounted = false;
static portMUX_TYPE s_sdcard_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_lifecycle_mutex;
static StaticSemaphore_t s_lifecycle_mutex_storage;
static uint32_t s_generation = 0;
static test_sdcard_status_t s_status = {
    .module = {
        .state = TEST_MODULE_STATE_UNINITIALIZED,
        .last_error = ESP_OK,
    },
};

static esp_err_t test_sdcard_lifecycle_enter(void)
{
    portENTER_CRITICAL(&s_sdcard_lock);
    if (s_lifecycle_mutex == NULL) {
        s_lifecycle_mutex = xSemaphoreCreateMutexStatic(&s_lifecycle_mutex_storage);
    }
    SemaphoreHandle_t mutex = s_lifecycle_mutex;
    portEXIT_CRITICAL(&s_sdcard_lock);

    ESP_RETURN_ON_FALSE(mutex != NULL, ESP_ERR_NO_MEM, TAG,
                        "create SD card lifecycle mutex failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE,
                        ESP_ERR_INVALID_STATE, TAG,
                        "take SD card lifecycle mutex failed");
    return ESP_OK;
}

static void test_sdcard_lifecycle_exit(void)
{
    if (s_lifecycle_mutex != NULL) {
        xSemaphoreGive(s_lifecycle_mutex);
    }
}

static uint32_t test_sdcard_checksum(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t checksum = 2166136261U;
    for (size_t i = 0; i < size; ++i) {
        checksum ^= bytes[i];
        checksum *= 16777619U;
    }
    return checksum;
}

static void test_sdcard_latch_error(esp_err_t error)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_sdcard_lock);
    s_status.last_error = error;
    s_status.test_passed = false;
    s_status.module.state = TEST_MODULE_STATE_ERROR;
    s_status.module.last_error = error;
    s_status.module.updated_at_us = now_us;
    portEXIT_CRITICAL(&s_sdcard_lock);
}

#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0))
#define TEST_SDCARD_WORKAROUND_HOSTED_SDMMC_HOST_INIT 1
#else
#define TEST_SDCARD_WORKAROUND_HOSTED_SDMMC_HOST_INIT 0
#endif

#if TEST_SDCARD_WORKAROUND_HOSTED_SDMMC_HOST_INIT
static esp_err_t test_sdcard_sdmmc_host_init_dummy(void)
{
    return ESP_OK;
}

static esp_err_t test_sdcard_sdmmc_host_deinit_dummy(void)
{
    return ESP_OK;
}
#endif

static esp_err_t test_sdcard_init_locked(void)
{
    esp_err_t ret = ESP_OK;

    if (s_mounted) {
        return ESP_OK;
    }

    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_sdcard_lock);
    s_status.initialized = false;
    s_status.mounted = false;
    s_status.test_passed = false;
    s_status.module.state = TEST_MODULE_STATE_INITIALIZING;
    s_status.module.last_error = ESP_OK;
    s_status.module.updated_at_us = now_us;
    portEXIT_CRITICAL(&s_sdcard_lock);

    if (s_pwr_ctrl_handle != NULL) {
        ret = sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl_handle);
        if (ret != ESP_OK) {
            test_sdcard_latch_error(ret);
            return ret;
        }
        s_pwr_ctrl_handle = NULL;
    }
    s_card = NULL;

    ESP_LOGI(TAG, "Initializing SD card (SDMMC peripheral, slot=%d width=%d)",
             SDMMC_HOST_SLOT_0, TEST_SDCARD_BUS_WIDTH);
    ESP_LOGI(TAG, "Hosted+SDMMC workaround: %s",
             TEST_SDCARD_WORKAROUND_HOSTED_SDMMC_HOST_INIT ? "enabled" : "disabled");

    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = TEST_SDCARD_PWR_LDO_CHAN,
    };
    ESP_GOTO_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_pwr_ctrl_handle),
                      err, TAG, "create on-chip LDO power control driver failed");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = s_pwr_ctrl_handle;
#if TEST_SDCARD_WORKAROUND_HOSTED_SDMMC_HOST_INIT
    // ESP-Hosted already manages the shared SDMMC host controller when C6 uses SDIO.
    host.init = test_sdcard_sdmmc_host_init_dummy;
    host.deinit = test_sdcard_sdmmc_host_deinit_dummy;
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = TEST_SDCARD_BUS_WIDTH;
    slot_config.clk = TEST_SDCARD_PIN_CLK;
    slot_config.cmd = TEST_SDCARD_PIN_CMD;
    slot_config.d0 = TEST_SDCARD_PIN_D0;
    slot_config.d1 = TEST_SDCARD_PIN_D1;
    slot_config.d2 = TEST_SDCARD_PIN_D2;
    slot_config.d3 = TEST_SDCARD_PIN_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = TEST_SDCARD_MAX_FILES,
        .allocation_unit_size = TEST_SDCARD_ALLOCATION_UNIT_SIZE,
    };

    ESP_LOGI(TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdmmc_mount(TEST_SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. If you want the card to be formatted, enable format option");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to acquire SDMMC host/card (%s). In Hosted+SDMMC mode, verify slot_0 wiring, pull-ups, and shared-host workaround",
                     esp_err_to_name(ret));
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). Make sure SD card lines have pull-up resistors",
                     esp_err_to_name(ret));
        }
        goto err;
    }

    ESP_LOGI(TAG, "Filesystem mounted");
    sdmmc_card_print_info(stdout, s_card);
    s_mounted = true;
    now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_sdcard_lock);
    s_generation++;
    s_status = (test_sdcard_status_t) {
        .module = {
            .state = TEST_MODULE_STATE_READY,
            .last_error = ESP_OK,
            .generation = s_generation,
            .updated_at_us = now_us,
        },
        .initialized = true,
        .mounted = true,
        .last_error = ESP_OK,
    };
    portEXIT_CRITICAL(&s_sdcard_lock);
    return ESP_OK;

err:
    s_card = NULL;
    if (s_pwr_ctrl_handle != NULL) {
        esp_err_t cleanup_ret = sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl_handle);
        if (cleanup_ret == ESP_OK) {
            s_pwr_ctrl_handle = NULL;
        } else {
            ESP_LOGE(TAG, "delete SD power control after init failure failed: %s",
                     esp_err_to_name(cleanup_ret));
        }
    }
    test_sdcard_latch_error(ret);
    return ret;
}

esp_err_t test_sdcard_init(void)
{
    ESP_RETURN_ON_ERROR(test_sdcard_lifecycle_enter(), TAG,
                        "enter SD card lifecycle failed");
    esp_err_t ret = test_sdcard_init_locked();
    test_sdcard_lifecycle_exit();
    return ret;
}

static esp_err_t test_sdcard_run_locked(void)
{
    if (!s_mounted) {
        ESP_LOGE(TAG, "SD card not mounted, call test_sdcard_init first");
        return ESP_ERR_INVALID_STATE;
    }

    static const char file_path[] = TEST_SDCARD_MOUNT_POINT "/test_sdcard.txt";
    static const char test_data[] = "JC1060P470 SD card test";
    const size_t test_size = sizeof(test_data) - 1;
    const uint32_t write_checksum = test_sdcard_checksum(test_data, test_size);
    size_t bytes_written = 0;
    size_t bytes_read = 0;
    uint32_t read_checksum = 0;
    FILE *f = NULL;
    esp_err_t ret = ESP_OK;

    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_sdcard_lock);
    s_status.run_count++;
    s_status.test_passed = false;
    s_status.last_bytes_written = 0;
    s_status.last_bytes_read = 0;
    s_status.last_write_checksum = write_checksum;
    s_status.last_read_checksum = 0;
    s_status.last_error = ESP_OK;
    s_status.module.state = TEST_MODULE_STATE_RUNNING;
    s_status.module.last_error = ESP_OK;
    s_status.module.updated_at_us = now_us;
    portEXIT_CRITICAL(&s_sdcard_lock);

    ESP_LOGI(TAG, "Opening file %s for writing", file_path);
    f = fopen(file_path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: errno=%d", errno);
        ret = ESP_FAIL;
        goto cleanup;
    }
    bytes_written = fwrite(test_data, 1, test_size, f);
    if (bytes_written != test_size || ferror(f)) {
        ESP_LOGE(TAG, "Failed to write complete test data: %u/%u bytes",
                 (unsigned)bytes_written, (unsigned)test_size);
        ret = ESP_FAIL;
        goto cleanup;
    }
    if (fflush(f) != 0) {
        ESP_LOGE(TAG, "Failed to flush test file: errno=%d", errno);
        ret = ESP_FAIL;
        goto cleanup;
    }
    if (fsync(fileno(f)) != 0) {
        ESP_LOGE(TAG, "Failed to sync test file: errno=%d", errno);
        ret = ESP_FAIL;
        goto cleanup;
    }
    if (fclose(f) != 0) {
        f = NULL;
        ESP_LOGE(TAG, "Failed to close test file after writing: errno=%d", errno);
        ret = ESP_FAIL;
        goto cleanup;
    }
    f = NULL;
    ESP_LOGI(TAG, "File written");

    ESP_LOGI(TAG, "Reading file %s", file_path);
    f = fopen(file_path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading: errno=%d", errno);
        ret = ESP_FAIL;
        goto cleanup;
    }
    uint8_t read_buffer[64] = {0};
    bytes_read = fread(read_buffer, 1, sizeof(read_buffer), f);
    if (ferror(f)) {
        ESP_LOGE(TAG, "Failed to read test file");
        ret = ESP_FAIL;
        goto cleanup;
    }
    if (fclose(f) != 0) {
        f = NULL;
        ESP_LOGE(TAG, "Failed to close test file after reading: errno=%d", errno);
        ret = ESP_FAIL;
        goto cleanup;
    }
    f = NULL;
    read_checksum = test_sdcard_checksum(read_buffer, bytes_read);

    if (bytes_read != test_size || read_checksum != write_checksum ||
        memcmp(read_buffer, test_data, test_size) != 0) {
        ESP_LOGE(TAG, "Data mismatch: size=%u/%u checksum=%08" PRIx32 "/%08" PRIx32,
                 (unsigned)bytes_read, (unsigned)test_size, read_checksum, write_checksum);
        ret = ESP_ERR_INVALID_CRC;
        goto cleanup;
    }

    if (unlink(file_path) != 0) {
        ESP_LOGE(TAG, "Failed to remove test file: errno=%d", errno);
        ret = ESP_FAIL;
        goto cleanup;
    }

    now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_sdcard_lock);
    s_status.test_passed = true;
    s_status.last_bytes_written = bytes_written;
    s_status.last_bytes_read = bytes_read;
    s_status.last_write_checksum = write_checksum;
    s_status.last_read_checksum = read_checksum;
    s_status.last_error = ESP_OK;
    s_status.module.state = TEST_MODULE_STATE_READY;
    s_status.module.last_error = ESP_OK;
    s_status.module.updated_at_us = now_us;
    portEXIT_CRITICAL(&s_sdcard_lock);
    ESP_LOGI(TAG, "Test file removed, SD card read/write test passed");
    return ESP_OK;

cleanup:
    if (f != NULL && fclose(f) != 0) {
        ESP_LOGE(TAG, "Failed to close test file during cleanup: errno=%d", errno);
    }
    if (unlink(file_path) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "Failed to remove test file during cleanup: errno=%d", errno);
    }
    now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_sdcard_lock);
    s_status.last_bytes_written = bytes_written;
    s_status.last_bytes_read = bytes_read;
    s_status.last_write_checksum = write_checksum;
    s_status.last_read_checksum = read_checksum;
    portEXIT_CRITICAL(&s_sdcard_lock);
    test_sdcard_latch_error(ret);
    return ret;
}

esp_err_t test_sdcard_run(void)
{
    ESP_RETURN_ON_ERROR(test_sdcard_lifecycle_enter(), TAG,
                        "enter SD card lifecycle failed");
    esp_err_t ret = test_sdcard_run_locked();
    test_sdcard_lifecycle_exit();
    return ret;
}

esp_err_t test_sdcard_get_status(test_sdcard_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != NULL, ESP_ERR_INVALID_ARG, TAG, "out_status is NULL");

    portENTER_CRITICAL(&s_sdcard_lock);
    *out_status = s_status;
    portEXIT_CRITICAL(&s_sdcard_lock);
    return ESP_OK;
}

static esp_err_t test_sdcard_deinit_locked(void)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_sdcard_lock);
    s_status.module.state = TEST_MODULE_STATE_STOPPING;
    s_status.module.updated_at_us = now_us;
    portEXIT_CRITICAL(&s_sdcard_lock);

    if (s_mounted) {
        esp_err_t ret = esp_vfs_fat_sdcard_unmount(TEST_SDCARD_MOUNT_POINT, s_card);
        if (ret != ESP_OK) {
            test_sdcard_latch_error(ret);
            return ret;
        }
        s_mounted = false;
        s_card = NULL;
        now_us = esp_timer_get_time();
        portENTER_CRITICAL(&s_sdcard_lock);
        s_status.initialized = false;
        s_status.mounted = false;
        s_status.module.updated_at_us = now_us;
        portEXIT_CRITICAL(&s_sdcard_lock);
    }

    if (s_pwr_ctrl_handle != NULL) {
        esp_err_t ret = sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl_handle);
        if (ret != ESP_OK) {
            test_sdcard_latch_error(ret);
            return ret;
        }
        s_pwr_ctrl_handle = NULL;
    }

    now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_sdcard_lock);
    s_status = (test_sdcard_status_t) {
        .module = {
            .state = TEST_MODULE_STATE_UNINITIALIZED,
            .last_error = ESP_OK,
            .generation = s_generation,
            .updated_at_us = now_us,
        },
    };
    portEXIT_CRITICAL(&s_sdcard_lock);
    ESP_LOGI(TAG, "Card unmounted");
    return ESP_OK;
}

esp_err_t test_sdcard_deinit(void)
{
    ESP_RETURN_ON_ERROR(test_sdcard_lifecycle_enter(), TAG,
                        "enter SD card lifecycle failed");
    esp_err_t ret = test_sdcard_deinit_locked();
    test_sdcard_lifecycle_exit();
    return ret;
}
