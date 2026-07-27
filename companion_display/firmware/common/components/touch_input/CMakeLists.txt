idf_component_register(SRCS "touch_input.c"
                       INCLUDE_DIRS "include"
                       REQUIRES companion_board
                       PRIV_REQUIRES esp_timer)
