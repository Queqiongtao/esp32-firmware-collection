idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES board_amoled_1_8 spi_flash esp_hw_support esp_lcd esp_lcd_touch)
