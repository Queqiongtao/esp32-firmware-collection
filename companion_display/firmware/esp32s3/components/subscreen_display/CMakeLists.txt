idf_component_register(
    SRCS "subscreen_display.c"
    INCLUDE_DIRS "include"
    REQUIRES board_amoled_1_8 esp_lcd esp_timer espressif__esp_jpeg
)
