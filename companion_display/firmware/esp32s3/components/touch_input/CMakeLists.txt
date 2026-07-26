idf_component_register(SRCS "touch_input.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board_amoled_1_8
                       PRIV_REQUIRES esp_timer)
