idf_component_register(SRCS "mipi_dsi_lcd_example_main.c" "lvgl_demo_ui.c"
                       INCLUDE_DIRS "."
                       REQUIRES lvgl
                       PRIV_REQUIRES esp_driver_gpio
                                     esp_driver_ledc
                                     esp_hw_support
                                     esp_timer
                                     esp_lcd
                                     esp_common
                                     log
                                     efuse
                                     esp_lcd_ili9881c
                                     esp_lcd_jd9165)
