#include "display_font.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "lvgl_demo_font_patch.h"
#include "libs/freetype/lv_freetype.h"
#include "libs/freetype/lv_freetype_private.h"
#include "misc/lv_log.h"

static const char *TAG = "display_font";
static const char *FONT_PARTITION_LABEL = "font";
static const char *FONT_BASE_PATH = "/spiffs";
static const char *FONT_LVGL_PREFIX = "A:";
#define DISPLAY_FONT_SAMPLE_CACHE_COUNT 8

static bool s_font_fs_mounted;
static bool s_freetype_owned;
static lv_font_t *s_ui_fonts[DISPLAY_FONT_ROLE_COUNT];
static const char *s_font_lvgl_path;
static display_font_diagnostics_t s_font_diag = {
    .state = DISPLAY_FONT_DIAG_NOT_RUN,
    .font_file_name = CONFIG_EXAMPLE_FONT_FILE_NAME,
};

typedef struct {
    uint16_t size_px;
    uint32_t last_used;
    lv_font_t *font;
} display_font_sample_cache_entry_t;

static display_font_sample_cache_entry_t s_sample_font_cache[DISPLAY_FONT_SAMPLE_CACHE_COUNT];
static uint32_t s_sample_font_use_counter;

static const uint16_t s_role_font_sizes[DISPLAY_FONT_ROLE_COUNT] = {
    [DISPLAY_FONT_ROLE_SMALL] = 16,
    [DISPLAY_FONT_ROLE_BODY] = 16,
    [DISPLAY_FONT_ROLE_CARD_TITLE] = 18,
    [DISPLAY_FONT_ROLE_TITLE] = 22,
};

static void display_font_log_heap(const char *stage)
{
    ESP_LOGI(TAG,
             "%s heap: default_free=%zu default_biggest=%zu internal_free=%zu internal_biggest=%zu spiram_free=%zu spiram_biggest=%zu",
             stage,
             heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

#if LV_USE_LOG
static void display_font_lvgl_log_cb(lv_log_level_t level, const char *buf)
{
    size_t len = strlen(buf);
    char line[256];

    if (len >= sizeof(line)) {
        len = sizeof(line) - 1;
    }
    memcpy(line, buf, len);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }
    line[len] = '\0';

    if (level <= LV_LOG_LEVEL_INFO) {
        ESP_LOGI("lvgl", "%s", line);
    } else if (level == LV_LOG_LEVEL_WARN) {
        ESP_LOGW("lvgl", "%s", line);
    } else {
        ESP_LOGE("lvgl", "%s", line);
    }
}
#endif

static void display_font_register_lvgl_log(void)
{
#if LV_USE_LOG
    static bool s_lvgl_log_registered;

    if (!s_lvgl_log_registered) {
        lv_log_register_print_cb(display_font_lvgl_log_cb);
        s_lvgl_log_registered = true;
        ESP_LOGI(TAG, "LVGL log callback registered");
    }
#endif
}

static void display_font_diag_set_stage(const char *stage)
{
    snprintf(s_font_diag.stage, sizeof(s_font_diag.stage), "%s", stage != NULL ? stage : "--");
}

static void display_font_diag_set_state(display_font_diag_state_t state, const char *stage, const char *error)
{
    s_font_diag.state = state;
    display_font_diag_set_stage(stage);
    if (error != NULL) {
        if (error != s_font_diag.first_error) {
            snprintf(s_font_diag.first_error, sizeof(s_font_diag.first_error), "%s", error);
        }
    } else {
        s_font_diag.first_error[0] = '\0';
    }
}

static void display_font_diag_prepare(void)
{
    memset(&s_font_diag, 0, sizeof(s_font_diag));
    s_font_diag.state = DISPLAY_FONT_DIAG_NOT_RUN;
    s_font_diag.font_file_name = CONFIG_EXAMPLE_FONT_FILE_NAME;
    display_font_diag_set_stage("init");
    for (display_font_role_t role = DISPLAY_FONT_ROLE_SMALL; role < DISPLAY_FONT_ROLE_COUNT; role++) {
        s_font_diag.role_sizes[role] = s_role_font_sizes[role];
    }
}

static void display_font_delete_fonts(void)
{
    for (display_font_role_t role = DISPLAY_FONT_ROLE_SMALL; role < DISPLAY_FONT_ROLE_COUNT; role++) {
        if (s_ui_fonts[role] != NULL) {
            lv_freetype_font_delete(s_ui_fonts[role]);
            s_ui_fonts[role] = NULL;
        }
    }

    for (size_t i = 0; i < DISPLAY_FONT_SAMPLE_CACHE_COUNT; i++) {
        if (s_sample_font_cache[i].font != NULL) {
            lv_freetype_font_delete(s_sample_font_cache[i].font);
            s_sample_font_cache[i].font = NULL;
        }
        s_sample_font_cache[i].size_px = 0;
        s_sample_font_cache[i].last_used = 0;
    }
    s_sample_font_use_counter = 0;
}

static esp_err_t display_font_mount_fs(void)
{
    size_t total = 0;
    size_t used = 0;

    display_font_diag_set_stage("mount");
    if (s_font_fs_mounted) {
        ESP_LOGI(TAG, "mount skip: partition=%s base=%s already mounted", FONT_PARTITION_LABEL, FONT_BASE_PATH);
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = FONT_BASE_PATH,
        .partition_label = FONT_PARTITION_LABEL,
        .max_files = 2,
        .format_if_mount_failed = false,
    };

    ESP_LOGI(TAG, "mount begin: partition=%s base=%s max_files=%d",
             FONT_PARTITION_LABEL, FONT_BASE_PATH, conf.max_files);
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    ESP_RETURN_ON_ERROR(ret, TAG, "register font spiffs failed");

    s_font_fs_mounted = true;
    ret = esp_spiffs_info(FONT_PARTITION_LABEL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "mount ok: partition=%s total=%zu used=%zu free=%zu",
                 FONT_PARTITION_LABEL, total, used, total >= used ? total - used : 0);
    } else {
        ESP_LOGW(TAG, "mount ok but spiffs info failed: partition=%s ret=%s",
                 FONT_PARTITION_LABEL, esp_err_to_name(ret));
    }
    return ESP_OK;
}

static esp_err_t display_font_check_lvgl_fs(const char *font_path)
{
    lv_fs_file_t file;
    uint8_t header[4] = {0};
    uint32_t bytes_read = 0;

    display_font_diag_set_stage("lvfs");
    ESP_LOGI(TAG, "lvfs check begin: path=%s", font_path);
    lv_fs_res_t fs_res = lv_fs_open(&file, font_path, LV_FS_MODE_RD);
    if (fs_res != LV_FS_RES_OK) {
        snprintf(s_font_diag.first_error, sizeof(s_font_diag.first_error),
                 "lv_fs_open=%d path=%s", (int)fs_res, font_path);
        ESP_LOGE(TAG, "LVGL fs open failed: %s", s_font_diag.first_error);
        return ESP_FAIL;
    }

    fs_res = lv_fs_read(&file, header, sizeof(header), &bytes_read);
    lv_fs_close(&file);
    if (fs_res != LV_FS_RES_OK || bytes_read != sizeof(header)) {
        snprintf(s_font_diag.first_error, sizeof(s_font_diag.first_error),
                 "lv_fs_read=%d bytes=%" PRIu32, (int)fs_res, bytes_read);
        ESP_LOGE(TAG, "LVGL fs read failed: %s", s_font_diag.first_error);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "lvfs font open ok: %s header=%02x %02x %02x %02x",
             font_path, header[0], header[1], header[2], header[3]);
    return ESP_OK;
}

static esp_err_t display_font_check_file(const char **font_path_out)
{
    static char host_font_path[96];
    static char lvgl_font_path[98];
    struct stat st = {0};

    display_font_diag_set_stage("file");
    snprintf(host_font_path, sizeof(host_font_path), "%s/%s", FONT_BASE_PATH, CONFIG_EXAMPLE_FONT_FILE_NAME);

    int rc = stat(host_font_path, &st);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_ERR_NOT_FOUND, TAG, "font file missing: %s", host_font_path);
    ESP_RETURN_ON_FALSE(st.st_size > 0, ESP_ERR_INVALID_SIZE, TAG, "font file is empty: %s", host_font_path);

    snprintf(lvgl_font_path, sizeof(lvgl_font_path), "%s/%s", FONT_LVGL_PREFIX, CONFIG_EXAMPLE_FONT_FILE_NAME);
    s_font_diag.font_file_name = lvgl_font_path;
    s_font_lvgl_path = lvgl_font_path;
    *font_path_out = lvgl_font_path;
    ESP_LOGI(TAG, "file check ok: host=%s size=%jd lvgl=%s",
             host_font_path, (intmax_t)st.st_size, lvgl_font_path);
    return ESP_OK;
}

static esp_err_t display_font_init_freetype(void)
{
    lv_freetype_context_t *ctx_before = lv_freetype_get_context();

    if (ctx_before != NULL && ctx_before->library != NULL && ctx_before->cache_node_cache != NULL) {
        ESP_LOGW(TAG, "ft_init skip: already initialized ctx=%p library=%p cache=%p",
                 ctx_before, ctx_before->library, ctx_before->cache_node_cache);
        return ESP_OK;
    }

    display_font_diag_set_stage("ft_init");
    if (ctx_before != NULL) {
        snprintf(s_font_diag.first_error, sizeof(s_font_diag.first_error),
                 "partial ctx=%p library=%p cache=%p",
                 ctx_before, ctx_before->library, ctx_before->cache_node_cache);
        ESP_LOGE(TAG, "ft_init blocked: %s", s_font_diag.first_error);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ft_init begin: glyph_cache=%u ctx_before=%p",
             (unsigned int)CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT,
             ctx_before);
    display_font_log_heap("ft_init begin");
    lv_result_t result = lv_freetype_init(CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT);
    lv_freetype_context_t *ctx_after = lv_freetype_get_context();
    if (result != LV_RESULT_OK) {
        snprintf(s_font_diag.first_error, sizeof(s_font_diag.first_error),
                 "lv_freetype_init=%d ctx_after=%p library=%p cache=%p",
                 (int)result,
                 ctx_after,
                 ctx_after != NULL ? ctx_after->library : NULL,
                 ctx_after != NULL ? ctx_after->cache_node_cache : NULL);
        ESP_LOGE(TAG, "freetype init failed: %s", s_font_diag.first_error);
        display_font_log_heap("ft_init failed");
        if (ctx_after != NULL) {
            ESP_LOGW(TAG, "ft_init cleanup partial context: ctx=%p", ctx_after);
            lv_freetype_uninit();
        }
        return ESP_FAIL;
    }

    if (ctx_after == NULL || ctx_after->library == NULL || ctx_after->cache_node_cache == NULL) {
        snprintf(s_font_diag.first_error, sizeof(s_font_diag.first_error),
                 "invalid ctx_after=%p library=%p cache=%p",
                 ctx_after,
                 ctx_after != NULL ? ctx_after->library : NULL,
                 ctx_after != NULL ? ctx_after->cache_node_cache : NULL);
        ESP_LOGE(TAG, "freetype init produced invalid context: %s", s_font_diag.first_error);
        if (ctx_after != NULL) {
            ESP_LOGW(TAG, "ft_init cleanup invalid context: ctx=%p", ctx_after);
            lv_freetype_uninit();
        }
        return ESP_FAIL;
    }

    s_freetype_owned = true;
    ESP_LOGI(TAG, "freetype init ok: glyph_cache=%u ctx=%p library=%p cache=%p",
             (unsigned int)CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT,
             ctx_after,
             ctx_after->library,
             ctx_after->cache_node_cache);
    display_font_log_heap("ft_init ok");
    return ESP_OK;
}

static esp_err_t display_font_init_backend(const char *font_path)
{
    display_font_diag_set_stage("create");
    ESP_LOGI(TAG, "create begin: path=%s roles=%d", font_path, DISPLAY_FONT_ROLE_COUNT);
    for (display_font_role_t role = DISPLAY_FONT_ROLE_SMALL; role < DISPLAY_FONT_ROLE_COUNT; role++) {
        if (s_ui_fonts[role] != NULL) {
            ESP_LOGI(TAG, "create skip: role=%d size=%u already exists",
                     role, (unsigned int)s_role_font_sizes[role]);
            continue;
        }

        ESP_LOGI(TAG, "create role begin: role=%d size=%u",
                 role,
                 (unsigned int)s_role_font_sizes[role]);
        display_font_log_heap("create role begin");
        s_ui_fonts[role] = lv_freetype_font_create(font_path,
                                                   LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                                   s_role_font_sizes[role],
                                                   LV_FREETYPE_FONT_STYLE_NORMAL);
        if (s_ui_fonts[role] == NULL) {
            display_font_delete_fonts();
            snprintf(s_font_diag.first_error, sizeof(s_font_diag.first_error),
                     "role=%d size=%u",
                     role,
                     (unsigned int)s_role_font_sizes[role]);
            ESP_LOGE(TAG, "create ui font failed: %s path=%s", s_font_diag.first_error, font_path);
            display_font_log_heap("create role failed");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "create role ok: role=%d size=%u font=%p",
                 role, (unsigned int)s_role_font_sizes[role], s_ui_fonts[role]);
        display_font_log_heap("create role ok");
    }

    return ESP_OK;
}

static esp_err_t display_font_run_diagnostics(void)
{
    display_font_diag_set_stage("ready");
    ESP_LOGI(TAG, "font role check begin: roles=%d", DISPLAY_FONT_ROLE_COUNT);
    for (display_font_role_t role = DISPLAY_FONT_ROLE_SMALL; role < DISPLAY_FONT_ROLE_COUNT; role++) {
        const lv_font_t *font = s_ui_fonts[role];
        if (font == NULL) {
            snprintf(s_font_diag.first_error, sizeof(s_font_diag.first_error), "role %d missing", role);
            s_font_diag.role_fail_count++;
            s_font_diag.state = DISPLAY_FONT_DIAG_ERROR;
            return ESP_FAIL;
        }

        s_font_diag.role_ready_count++;
    }

    if (s_font_diag.role_fail_count > 0) {
        s_font_diag.state = DISPLAY_FONT_DIAG_ERROR;
        ESP_LOGE(TAG, "font role check failed: ready=%" PRIu32 " fail=%" PRIu32 " first=%s",
                 s_font_diag.role_ready_count,
                 s_font_diag.role_fail_count,
                 s_font_diag.first_error);
        return ESP_FAIL;
    }

    s_font_diag.state = DISPLAY_FONT_DIAG_OK;
    ESP_LOGI(TAG, "font role check ok: ready=%" PRIu32 " fail=%" PRIu32,
             s_font_diag.role_ready_count,
             s_font_diag.role_fail_count);
    return ESP_OK;
}

esp_err_t display_font_init(void)
{
    const char *font_path = NULL;
    esp_err_t ret = ESP_OK;

    display_font_diag_prepare();
    display_font_register_lvgl_log();
    ESP_LOGI(TAG, "init begin: font=%s host_base=%s lvgl_prefix=%s fs_cache=%u glyph_cache=%u",
             CONFIG_EXAMPLE_FONT_FILE_NAME,
             FONT_BASE_PATH,
             FONT_LVGL_PREFIX,
             (unsigned int)CONFIG_LV_FS_STDIO_CACHE_SIZE,
             (unsigned int)CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT);
    display_font_log_heap("init begin");

    ret = display_font_mount_fs();
    if (ret != ESP_OK) {
        display_font_diag_set_state(DISPLAY_FONT_DIAG_FALLBACK, "mount", "mount failed");
        ESP_RETURN_ON_ERROR(ret, TAG, "mount font fs failed");
    }

    ret = display_font_check_file(&font_path);
    if (ret != ESP_OK) {
        display_font_diag_set_state(DISPLAY_FONT_DIAG_FALLBACK, "file", "file invalid");
        ESP_RETURN_ON_ERROR(ret, TAG, "validate font file failed");
    }

    ret = display_font_check_lvgl_fs(font_path);
    if (ret != ESP_OK) {
        display_font_diag_set_state(DISPLAY_FONT_DIAG_FALLBACK, "lvfs", s_font_diag.first_error);
        ESP_RETURN_ON_ERROR(ret, TAG, "validate LVGL font path failed");
    }

    ESP_LOGI(TAG, "display_font build: lvgl=%d.%d.%d path=%s local-ft-init",
             LVGL_VERSION_MAJOR,
             LVGL_VERSION_MINOR,
             LVGL_VERSION_PATCH,
             font_path);

    ret = display_font_init_freetype();
    if (ret != ESP_OK) {
        display_font_diag_set_state(DISPLAY_FONT_DIAG_FALLBACK, "ft_init", s_font_diag.first_error);
        ESP_RETURN_ON_ERROR(ret, TAG, "init freetype failed");
    }

    ret = display_font_init_backend(font_path);
    if (ret != ESP_OK) {
        if (s_font_diag.first_error[0] == '\0') {
            display_font_diag_set_state(DISPLAY_FONT_DIAG_FALLBACK, "create", "backend failed");
        } else {
            display_font_diag_set_state(DISPLAY_FONT_DIAG_FALLBACK, "create", s_font_diag.first_error);
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "init font backend failed");
    }

    ret = display_font_run_diagnostics();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "font diagnostics failed: %s", s_font_diag.first_error);
        s_font_diag.state = DISPLAY_FONT_DIAG_ERROR;
    }

    ESP_LOGI(TAG, "FreeType diagnostics ready: %s sizes=%u/%u/%u/%u roles_ready=%" PRIu32
             " ui_font=static_patch",
             font_path,
             (unsigned int)s_role_font_sizes[DISPLAY_FONT_ROLE_SMALL],
             (unsigned int)s_role_font_sizes[DISPLAY_FONT_ROLE_BODY],
             (unsigned int)s_role_font_sizes[DISPLAY_FONT_ROLE_CARD_TITLE],
             (unsigned int)s_role_font_sizes[DISPLAY_FONT_ROLE_TITLE],
             s_font_diag.role_ready_count);
    display_font_log_heap("font ready");
    return ESP_OK;
}

const lv_font_t *display_font_get_ui_font(void)
{
    return display_font_get_role_font(DISPLAY_FONT_ROLE_BODY);
}

const lv_font_t *display_font_get_role_font(display_font_role_t role)
{
    (void)role;
    return &lvgl_demo_font_patch_16;
}

const lv_font_t *display_font_get_freetype_role_font(display_font_role_t role)
{
    if (role >= DISPLAY_FONT_ROLE_SMALL && role < DISPLAY_FONT_ROLE_COUNT && s_ui_fonts[role] != NULL) {
        return s_ui_fonts[role];
    }

    return NULL;
}

const lv_font_t *display_font_get_freetype_sample_font(uint16_t size_px)
{
    if (!display_font_is_freetype_ready() || s_font_lvgl_path == NULL) {
        return NULL;
    }

    if (size_px < DISPLAY_FONT_SAMPLE_SIZE_MIN) {
        size_px = DISPLAY_FONT_SAMPLE_SIZE_MIN;
    } else if (size_px > DISPLAY_FONT_SAMPLE_SIZE_MAX) {
        size_px = DISPLAY_FONT_SAMPLE_SIZE_MAX;
    }
    size_px = (uint16_t)(((size_px + (DISPLAY_FONT_SAMPLE_SIZE_STEP / 2)) / DISPLAY_FONT_SAMPLE_SIZE_STEP) *
                         DISPLAY_FONT_SAMPLE_SIZE_STEP);

    display_font_sample_cache_entry_t *free_entry = NULL;
    display_font_sample_cache_entry_t *lru_entry = &s_sample_font_cache[0];
    for (size_t i = 0; i < DISPLAY_FONT_SAMPLE_CACHE_COUNT; i++) {
        display_font_sample_cache_entry_t *entry = &s_sample_font_cache[i];
        if (entry->font != NULL && entry->size_px == size_px) {
            entry->last_used = ++s_sample_font_use_counter;
            return entry->font;
        }
        if (entry->font == NULL && free_entry == NULL) {
            free_entry = entry;
        }
        if (entry->last_used < lru_entry->last_used) {
            lru_entry = entry;
        }
    }

    display_font_sample_cache_entry_t *entry = free_entry != NULL ? free_entry : lru_entry;
    if (entry->font != NULL) {
        ESP_LOGI(TAG, "sample font cache evict: size=%u font=%p",
                 (unsigned int)entry->size_px, entry->font);
        lv_freetype_font_delete(entry->font);
        entry->font = NULL;
    }

    ESP_LOGI(TAG, "sample font create begin: size=%u path=%s", (unsigned int)size_px, s_font_lvgl_path);
    entry->font = lv_freetype_font_create(s_font_lvgl_path,
                                          LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                          size_px,
                                          LV_FREETYPE_FONT_STYLE_NORMAL);
    if (entry->font == NULL) {
        entry->size_px = 0;
        entry->last_used = 0;
        snprintf(s_font_diag.first_error, sizeof(s_font_diag.first_error), "sample size=%u", (unsigned int)size_px);
        ESP_LOGE(TAG, "sample font create failed: %s path=%s", s_font_diag.first_error, s_font_lvgl_path);
        return NULL;
    }

    entry->size_px = size_px;
    entry->last_used = ++s_sample_font_use_counter;
    ESP_LOGI(TAG, "sample font create ok: size=%u font=%p", (unsigned int)size_px, entry->font);
    return entry->font;
}

bool display_font_is_freetype_ready(void)
{
    return s_font_diag.state == DISPLAY_FONT_DIAG_OK;
}

const display_font_diagnostics_t *display_font_get_diagnostics(void)
{
    return &s_font_diag;
}

void display_font_deinit(void)
{
    display_font_delete_fonts();

    if (s_freetype_owned) {
        lv_freetype_uninit();
        s_freetype_owned = false;
    }

    if (s_font_fs_mounted) {
        esp_vfs_spiffs_unregister(FONT_PARTITION_LABEL);
        s_font_fs_mounted = false;
    }
}
