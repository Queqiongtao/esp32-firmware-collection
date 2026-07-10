idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES subscreen_display usb_extend_screen spi_flash esp_hw_support)
