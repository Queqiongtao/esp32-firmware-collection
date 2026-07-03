idf_component_register(
    SRCS "board_amoled_1_8.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c esp_driver_gpio esp_driver_ledc esp_lcd
)
