idf_component_register(SRCS "subscreen_display.c"
                       INCLUDE_DIRS "include"
                       REQUIRES companion_board esp_lcd
                       PRIV_REQUIRES esp_driver_jpeg esp_driver_ppa esp_timer)
