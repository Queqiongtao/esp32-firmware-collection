idf_component_register(
    SRCS "usb_extend_screen.c"
    INCLUDE_DIRS "include" "tusb"
    REQUIRES subscreen_display espressif__tinyusb
)

idf_component_get_property(tinyusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tinyusb_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/tusb")
target_sources(${tinyusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/tusb/usb_descriptors.c")

cmake_policy(SET CMP0079 NEW)
target_link_libraries(${tinyusb_lib} PRIVATE ${COMPONENT_LIB})
