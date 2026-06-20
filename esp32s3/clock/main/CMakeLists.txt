set(MAIN_SRCS
    "main.cpp"
    "rtc_time_sync.cpp"
    "imu_demo.cpp"
    "lv_font_clock_96.c"
    "clock_theme_minimal.cpp"
    "clock_theme_segment.cpp"
    "lv_font_seg_96.c"
    "lv_font_seg_36.c"
    "clock_theme_analog.cpp"
    "clock_theme_outline_analog.cpp"
    "clock_theme_mooncat.cpp"
    "clock_theme_m5_watch.cpp"
    "m5_assets/images/watch_face/classic/classic_hour_hand.c"
    "m5_assets/images/watch_face/classic/classic_minute_hand.c"
    "m5_assets/images/watch_face/classic/classic_second_hand.c"
    "m5_assets/images/watch_face/big_number/big_number_0.c"
    "m5_assets/images/watch_face/big_number/big_number_1.c"
    "m5_assets/images/watch_face/big_number/big_number_2.c"
    "m5_assets/images/watch_face/big_number/big_number_3.c"
    "m5_assets/images/watch_face/big_number/big_number_4.c"
    "m5_assets/images/watch_face/big_number/big_number_5.c"
    "m5_assets/images/watch_face/big_number/big_number_6.c"
    "m5_assets/images/watch_face/big_number/big_number_7.c"
    "m5_assets/images/watch_face/big_number/big_number_8.c"
    "m5_assets/images/watch_face/big_number/big_number_9.c"
    "m5_assets/fonts/CommissionerMedium64.c"
    "m5_assets/fonts/CommissionerMedium108.c"
    "m5_assets/fonts/lv_font_maple_mono_medium_24.c"
    "m5_assets/fonts/lv_font_maple_mono_medium_28.c")

set(MAIN_INCLUDE_DIRS ".")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    set(MOONCAT_GIF ${CMAKE_CURRENT_SOURCE_DIR}/assets/mooncat.gif)
    set(MOONCAT_GENERATOR ${CMAKE_CURRENT_SOURCE_DIR}/mooncat_frames_gen.py)
    set(MOONCAT_FRAMES_C ${CMAKE_CURRENT_BINARY_DIR}/mooncat_frames.c)
    set(MOONCAT_FRAMES_H ${CMAKE_CURRENT_BINARY_DIR}/mooncat_frames.h)

    add_custom_command(
        OUTPUT ${MOONCAT_FRAMES_C} ${MOONCAT_FRAMES_H}
        COMMAND ${Python3_EXECUTABLE} ${MOONCAT_GENERATOR}
                --input ${MOONCAT_GIF}
                --output-c ${MOONCAT_FRAMES_C}
                --output-h ${MOONCAT_FRAMES_H}
        DEPENDS ${MOONCAT_GIF} ${MOONCAT_GENERATOR}
        VERBATIM)

    list(APPEND MAIN_SRCS ${MOONCAT_FRAMES_C})
    list(APPEND MAIN_INCLUDE_DIRS ${CMAKE_CURRENT_BINARY_DIR})
endif()

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS ${MAIN_INCLUDE_DIRS}
    REQUIRES attitude pcf85063 qmi8658 ui_physics button)

target_compile_options(${COMPONENT_LIB} PUBLIC -Wno-missing-field-initializers)

# Function to get component library
function(get_component_library component_name output_var)
    # Get the exact component name
    idf_build_get_property(build_components BUILD_COMPONENTS)
    set(TARGET_COMPONENT "")
    foreach(COMPONENT ${build_components})
        if(COMPONENT MATCHES "${component_name}" OR COMPONENT MATCHES "espressif__${component_name}")
            set(TARGET_COMPONENT ${COMPONENT})
            break()
        endif()
    endforeach()

    # Get the component library
    if(TARGET_COMPONENT STREQUAL "")
        message(FATAL_ERROR "Component '${component_name}' not found.")
    else()
        idf_component_get_property(COMPONENT_LIB ${TARGET_COMPONENT} COMPONENT_LIB)
        set(${output_var} ${COMPONENT_LIB} PARENT_SCOPE)
    endif()
endfunction()

get_component_library("lvgl" LVGL_LIB)
target_compile_options(
    ${LVGL_LIB}
    PUBLIC
        "-Wno-attributes"
)
