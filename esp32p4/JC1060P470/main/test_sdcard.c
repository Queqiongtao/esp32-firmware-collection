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
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>

static const char *TAG = "test_sdcard";

static sdmmc_card_t *s_card = NULL;
static sd_pwr_ctrl_handle_t s_pwr_ctrl_handle = NULL;
static bool s_mounted = false;

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

esp_err_t test_sdcard_init(void)
{
    esp_err_t ret = ESP_OK;

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
    return ESP_OK;

err:
    if (s_pwr_ctrl_handle) {
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl_handle);
        s_pwr_ctrl_handle = NULL;
    }
    return ret;
}

esp_err_t test_sdcard_run(void)
{
    if (!s_mounted) {
        ESP_LOGE(TAG, "SD card not mounted, call test_sdcard_init first");
        return ESP_ERR_INVALID_STATE;
    }

    const char *file_path = TEST_SDCARD_MOUNT_POINT "/test_sdcard.txt";
    const char *test_data = "JC1060P470 SD card test";

    ESP_LOGI(TAG, "Opening file %s for writing", file_path);
    FILE *f = fopen(file_path, "w");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "Failed to open file for writing");
    fprintf(f, "%s\n", test_data);
    fclose(f);
    ESP_LOGI(TAG, "File written");

    ESP_LOGI(TAG, "Reading file %s", file_path);
    f = fopen(file_path, "r");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "Failed to open file for reading");
    char line[64];
    char *got = fgets(line, sizeof(line), f);
    fclose(f);
    ESP_RETURN_ON_FALSE(got, ESP_FAIL, TAG, "Failed to read from file");

    char *pos = strchr(line, '\n');
    if (pos) {
        *pos = '\0';
    }
    ESP_LOGI(TAG, "Read from file: '%s'", line);

    if (strcmp(line, test_data) != 0) {
        ESP_LOGE(TAG, "Data mismatch: expected '%s', got '%s'", test_data, line);
        return ESP_FAIL;
    }

    unlink(file_path);
    ESP_LOGI(TAG, "Test file removed, SD card read/write test passed");
    return ESP_OK;
}

esp_err_t test_sdcard_deinit(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_vfs_fat_sdcard_unmount(TEST_SDCARD_MOUNT_POINT, s_card),
                        TAG, "unmount filesystem failed");
    s_mounted = false;
    s_card = NULL;

    if (s_pwr_ctrl_handle) {
        ESP_RETURN_ON_ERROR(sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl_handle),
                            TAG, "delete on-chip LDO power control driver failed");
        s_pwr_ctrl_handle = NULL;
    }

    ESP_LOGI(TAG, "Card unmounted");
    return ESP_OK;
}
