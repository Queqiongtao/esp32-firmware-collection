set(MAIN_SRCS
    "main.cpp"
    "clock_settings.cpp"
    "clock_settings_menu.cpp"
    "clock_touch_gesture.cpp"
    "rtc_time_sync.cpp"
    "imu_demo.cpp"
    "lv_font_clock_96.c"
    "lv_font_clock_144.c"
    "clock_theme_minimal.cpp"
    "clock_theme_segment.cpp"
    "lv_font_seg_96.c"
    "lv_font_seg_36.c"
    "clock_theme_analog.cpp"
    "clock_theme_outline_analog.cpp"
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

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS ${MAIN_INCLUDE_DIRS}
    REQUIRES pcf85063 qmi8658 ui_physics button nvs_flash esp_io_expander)

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
