idf_component_register(
    SRCS
        "board_amoled_1_8.cpp"
        "board_display.cpp"
        "board_touch_ft3168.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        esp_driver_i2c
        esp_driver_gpio
        esp_driver_spi
        esp_driver_i2s
        esp_driver_sdmmc
        esp_lcd
        espressif__esp_lcd_touch
        espressif__esp_lcd_touch_ft5x06
        espressif__esp_io_expander
        espressif__esp_io_expander_tca9554
        waveshare__esp32_s3_touch_amoled_1_8
        waveshare__esp_lcd_sh8601
        fatfs
        sdmmc
        espressif__esp_codec_dev
)
