/**
 * @file preview.cpp
 * 主机端主题预览：用 LVGL 官方桌面构建把每个时钟主题渲染成 PPM。
 *
 * 流程：lv_init -> 虚拟 466x466 RGB565 display -> 逐个主题 build/update/tick
 *      -> lv_snapshot_take(screen) -> RGB565 转 PPM。
 *
 * 注意：快照对象是 screen 而不是主题根，这样 M5 Simple 这类把元素挂在
 * screen 上的覆盖层也会被渲染出来。
 */

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "lvgl.h"
#include "clock_theme.h"

namespace {

constexpr int32_t DISPLAY_SIZE = 466;

uint8_t g_display_buffer[DISPLAY_SIZE * DISPLAY_SIZE * 2];

const clock_theme_t *const kThemes[] = {
    &g_clock_theme_minimal,
    &g_clock_theme_segment,
    &g_clock_theme_analog,
    &g_clock_theme_outline_analog,
    &g_clock_theme_m5_classic,
    &g_clock_theme_m5_simple,
    &g_clock_theme_m5_big_number,
    &g_clock_theme_m5_number_flow,
};

void display_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    LV_UNUSED(area);
    LV_UNUSED(px_map);
    lv_display_flush_ready(disp);
}

void write_ppm(const std::filesystem::path &path, const lv_draw_buf_t *buf)
{
    FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "failed to open %s\n", path.c_str());
        return;
    }

    std::fprintf(file, "P6\n%" PRId32 " %" PRId32 "\n255\n", buf->header.w, buf->header.h);

    std::vector<uint8_t> row(static_cast<size_t>(buf->header.w) * 3);
    for (int32_t y = 0; y < buf->header.h; ++y) {
        const auto *src = reinterpret_cast<const uint16_t *>(buf->data + static_cast<size_t>(y) * buf->header.stride);
        for (int32_t x = 0; x < buf->header.w; ++x) {
            const uint16_t pixel = src[x];
            row[static_cast<size_t>(x) * 3 + 0] = static_cast<uint8_t>(((pixel >> 11) & 0x1F) * 255 / 31);
            row[static_cast<size_t>(x) * 3 + 1] = static_cast<uint8_t>(((pixel >> 5) & 0x3F) * 255 / 63);
            row[static_cast<size_t>(x) * 3 + 2] = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }

    std::fclose(file);
}

struct tm make_time(int hour, int minute, int second)
{
    struct tm timeinfo = {};
    timeinfo.tm_year = 2026 - 1900;
    timeinfo.tm_mon = 10 - 1;
    timeinfo.tm_mday = 20;
    timeinfo.tm_hour = hour;
    timeinfo.tm_min = minute;
    timeinfo.tm_sec = second;
    std::mktime(&timeinfo); // 归一化并填充 tm_wday / tm_yday
    return timeinfo;
}

bool parse_time(const char *text, int *hour, int *minute, int *second)
{
    return std::sscanf(text, "%d:%d:%d", hour, minute, second) == 3;
}

void prepare_screen()
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

void render_theme(const clock_theme_t *theme,
                  const struct tm &timeinfo,
                  const std::filesystem::path &out_dir,
                  const std::string &time_tag)
{
    prepare_screen();
    lv_obj_t *screen = lv_screen_active();

    lv_obj_t *root = theme->build(screen);
    if (root == nullptr) {
        std::fprintf(stderr, "theme %s build returned NULL\n", theme->name);
        return;
    }

    if (theme->reset != nullptr) {
        theme->reset();
    }
    if (theme->update != nullptr) {
        theme->update(&timeinfo);
    }

    // 推进一次内部动画到结束状态，保证截图稳定可复现。
    if (theme->tick != nullptr) {
        const uint32_t target = lv_tick_get() + 5000;
        theme->tick(target);
        theme->tick(target + 1000);
    }

    lv_obj_update_layout(screen);

    lv_draw_buf_t *snapshot = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565);
    if (snapshot == nullptr) {
        std::fprintf(stderr, "theme %s snapshot failed\n", theme->name);
    } else {
        const std::string name = std::string(theme->name) + "_" + time_tag + ".ppm";
        write_ppm(out_dir / name, snapshot);
        lv_draw_buf_destroy(snapshot);
        std::printf("rendered %s\n", name.c_str());
    }

    if (theme->destroy_overlay != nullptr) {
        theme->destroy_overlay();
    }
    lv_obj_clean(screen);
}

} // namespace

int main(int argc, char **argv)
{
    setenv("TZ", "UTC", 1);
    tzset();

    std::vector<std::pair<int, int>> requested_times = {{10, 8}, {23, 59}};
    int second = 35;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            requested_times.clear();
            int hour = 0;
            int minute = 0;
            int sec = 0;
            if (!parse_time(argv[++i], &hour, &minute, &sec)) {
                std::fprintf(stderr, "invalid --time, expected HH:MM:SS\n");
                return 1;
            }
            requested_times.emplace_back(hour, minute);
            second = sec;
        }
    }

    const std::filesystem::path out_dir = std::filesystem::path(argv[0]).parent_path() / ".." / "out" / "ppm";
    std::filesystem::create_directories(out_dir);

    lv_init();

    lv_display_t *display = lv_display_create(DISPLAY_SIZE, DISPLAY_SIZE);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,
                           g_display_buffer,
                           nullptr,
                           sizeof(g_display_buffer),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, display_flush_cb);

    for (const auto &requested : requested_times) {
        char tag[16] = {};
        std::snprintf(tag, sizeof(tag), "%02d-%02d-%02d", requested.first, requested.second, second);
        const struct tm timeinfo = make_time(requested.first, requested.second, second);
        for (const clock_theme_t *theme : kThemes) {
            render_theme(theme, timeinfo, out_dir, tag);
        }
    }

    std::printf("preview finished, output dir: %s\n", out_dir.c_str());
    return 0;
}
