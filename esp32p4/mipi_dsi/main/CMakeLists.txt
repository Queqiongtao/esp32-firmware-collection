idf_component_register(SRCS "mipi_dsi_lcd_example_main.c"
                            "display_board.c"
                            "display_panel.c"
                            "display_lvgl.c"
                            "display_touch.c"
                            "lvgl_demo_ui.c"
                       INCLUDE_DIRS "."
                       REQUIRES lvgl
                       PRIV_REQUIRES esp_driver_gpio
                                     esp_driver_ledc
                                     esp_driver_i2c
                                     esp_hw_support
                                     esp_timer
                                     esp_lcd
                                     esp_common
                                     log
                                     efuse
                                     esp_lcd_jd9165
                                     esp_lcd_touch_gt911)
