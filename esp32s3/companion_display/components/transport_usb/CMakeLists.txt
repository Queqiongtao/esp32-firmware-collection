idf_component_register(SRCS "transport_usb.c"
                       INCLUDE_DIRS "include" "tusb"
                       REQUIRES frame_pipeline esp_hw_support espressif__tinyusb)

idf_component_get_property(tinyusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tinyusb_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/tusb")
target_sources(${tinyusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/tusb/usb_descriptors.c")
