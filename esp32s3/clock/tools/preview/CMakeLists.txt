cmake_minimum_required(VERSION 3.16)
project(esp_clock_preview C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(LVGL_DIR "${PROJECT_ROOT}/managed_components/lvgl__lvgl")
set(MAIN_DIR "${PROJECT_ROOT}/main")

if(NOT EXISTS "${LVGL_DIR}/lvgl.h")
    message(FATAL_ERROR "LVGL not found at ${LVGL_DIR}. Run ./dev.sh build once to fetch managed_components.")
endif()

# LVGL 主机端配置
set(LV_BUILD_CONF_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_PRIVATE_API OFF CACHE BOOL "" FORCE)

add_subdirectory(${LVGL_DIR} ${CMAKE_BINARY_DIR}/lvgl)

file(GLOB M5_IMAGES "${MAIN_DIR}/m5_assets/images/watch_face/*/*.c")
file(GLOB M5_FONTS "${MAIN_DIR}/m5_assets/fonts/*.c")

add_executable(preview
    preview.cpp
    ${MAIN_DIR}/clock_theme_minimal.cpp
    ${MAIN_DIR}/clock_theme_segment.cpp
    ${MAIN_DIR}/clock_theme_analog.cpp
    ${MAIN_DIR}/clock_theme_outline_analog.cpp
    ${MAIN_DIR}/clock_theme_m5_classic.cpp
    ${MAIN_DIR}/clock_theme_m5_simple.cpp
    ${MAIN_DIR}/clock_theme_m5_big_number.cpp
    ${MAIN_DIR}/clock_theme_m5_number_flow.cpp
    ${MAIN_DIR}/lv_font_clock_96.c
    ${MAIN_DIR}/lv_font_clock_144.c
    ${MAIN_DIR}/lv_font_seg_96.c
    ${MAIN_DIR}/lv_font_seg_36.c
    ${M5_FONTS}
    ${M5_IMAGES})

target_include_directories(preview PRIVATE ${MAIN_DIR})
target_link_libraries(preview PRIVATE lvgl m)

target_compile_options(preview PRIVATE -Wno-missing-field-initializers)
