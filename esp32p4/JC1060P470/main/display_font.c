#include "display_font.h"

#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "lvgl_demo_font_patch.h"
#include "libs/freetype/lv_freetype.h"

static const char *TAG = "display_font";
static const char *FONT_PARTITION_LABEL = "font";
static const char *FONT_BASE_PATH = "/spiffs";
static const char *FONT_LVGL_PREFIX = "A:";

static bool s_font_fs_mounted;
static bool s_freetype_ready;
static lv_font_t *s_ui_fonts[DISPLAY_FONT_ROLE_COUNT];

static const uint16_t s_role_font_sizes[DISPLAY_FONT_ROLE_COUNT] = {
    [DISPLAY_FONT_ROLE_SMALL] = 16,
    [DISPLAY_FONT_ROLE_BODY] = 16,
    [DISPLAY_FONT_ROLE_CARD_TITLE] = 18,
    [DISPLAY_FONT_ROLE_TITLE] = 22,
};

static void display_font_delete_fonts(void)
{
    for (display_font_role_t role = DISPLAY_FONT_ROLE_SMALL; role < DISPLAY_FONT_ROLE_COUNT; role++) {
        if (s_ui_fonts[role] != NULL) {
            lv_freetype_font_delete(s_ui_fonts[role]);
            s_ui_fonts[role] = NULL;
        }
    }
}

static esp_err_t display_font_mount_fs(void)
{
    if (s_font_fs_mounted) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = FONT_BASE_PATH,
        .partition_label = FONT_PARTITION_LABEL,
        .max_files = 2,
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    ESP_RETURN_ON_ERROR(ret, TAG, "register font spiffs failed");

    s_font_fs_mounted = true;
    return ESP_OK;
}

static esp_err_t display_font_check_file(const char **font_path_out)
{
    static char host_font_path[96];
    static char lvgl_font_path[98];
    struct stat st = {0};

    snprintf(host_font_path, sizeof(host_font_path), "%s/%s", FONT_BASE_PATH, CONFIG_EXAMPLE_FONT_FILE_NAME);

    int rc = stat(host_font_path, &st);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_ERR_NOT_FOUND, TAG, "font file missing: %s", host_font_path);
    ESP_RETURN_ON_FALSE(st.st_size > 0, ESP_ERR_INVALID_SIZE, TAG, "font file is empty: %s", host_font_path);

    snprintf(lvgl_font_path, sizeof(lvgl_font_path), "%s%s", FONT_LVGL_PREFIX, host_font_path);
    *font_path_out = lvgl_font_path;
    return ESP_OK;
}

static esp_err_t display_font_init_backend(const char *font_path)
{
    if (!s_freetype_ready) {
        lv_result_t result = lv_freetype_init(CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT);
        ESP_RETURN_ON_FALSE(result == LV_RESULT_OK, ESP_FAIL, TAG, "init freetype failed");
        s_freetype_ready = true;
    }

    for (display_font_role_t role = DISPLAY_FONT_ROLE_SMALL; role < DISPLAY_FONT_ROLE_COUNT; role++) {
        if (s_ui_fonts[role] != NULL) {
            continue;
        }

        s_ui_fonts[role] = lv_freetype_font_create(font_path,
                                                   LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                                   s_role_font_sizes[role],
                                                   LV_FREETYPE_FONT_STYLE_NORMAL);
        if (s_ui_fonts[role] == NULL) {
            display_font_delete_fonts();
            ESP_LOGE(TAG, "create ui font failed: role=%d size=%u", role, (unsigned int)s_role_font_sizes[role]);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

esp_err_t display_font_init(void)
{
    const char *font_path = NULL;

    ESP_RETURN_ON_ERROR(display_font_mount_fs(), TAG, "mount font fs failed");
    ESP_RETURN_ON_ERROR(display_font_check_file(&font_path), TAG, "validate font file failed");
    ESP_RETURN_ON_ERROR(display_font_init_backend(font_path), TAG, "init font backend failed");

    ESP_LOGI(TAG, "UI font ready: %s sizes=%u/%u/%u/%u",
             font_path,
             (unsigned int)s_role_font_sizes[DISPLAY_FONT_ROLE_SMALL],
             (unsigned int)s_role_font_sizes[DISPLAY_FONT_ROLE_BODY],
             (unsigned int)s_role_font_sizes[DISPLAY_FONT_ROLE_CARD_TITLE],
             (unsigned int)s_role_font_sizes[DISPLAY_FONT_ROLE_TITLE]);
    return ESP_OK;
}

const lv_font_t *display_font_get_ui_font(void)
{
    return display_font_get_role_font(DISPLAY_FONT_ROLE_BODY);
}

const lv_font_t *display_font_get_role_font(display_font_role_t role)
{
    if (role >= DISPLAY_FONT_ROLE_SMALL && role < DISPLAY_FONT_ROLE_COUNT && s_ui_fonts[role] != NULL) {
        return s_ui_fonts[role];
    }

    return &lvgl_demo_font_patch_16;
}

void display_font_deinit(void)
{
    display_font_delete_fonts();

    if (s_freetype_ready) {
        lv_freetype_uninit();
        s_freetype_ready = false;
    }

    if (s_font_fs_mounted) {
        esp_vfs_spiffs_unregister(FONT_PARTITION_LABEL);
        s_font_fs_mounted = false;
    }
}
