idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES board_amoled_1_8 block_physics spi_flash esp_hw_support esp_lcd)
