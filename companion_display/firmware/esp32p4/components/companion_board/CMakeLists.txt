idf_component_register(SRCS "companion_board.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd
                       PRIV_REQUIRES esp_lcd_jd9165 esp_driver_gpio esp_driver_i2c
                                     esp_driver_ledc esp_event esp_timer
                                     espressif__esp_lcd_touch_gt911
                                     espressif__esp_hosted)
