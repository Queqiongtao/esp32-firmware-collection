idf_component_register(SRCS "JC1060P470_main.c"
                            "display_board.c"
                            "display_panel.c"
                            "display_lvgl.c"
                            "display_touch.c"
                            "lvgl_demo_ui.c"
                            "test_sdcard.c"
                            "test_audio.c"
                            "test_usb.c"
                       INCLUDE_DIRS "."
                       REQUIRES lvgl
                       PRIV_REQUIRES esp_driver_gpio
                                     esp_driver_ledc
                                     esp_driver_i2c
                                     esp_driver_i2s
                                     esp_driver_sdmmc
                                     esp_hw_support
                                     esp_timer
                                     esp_lcd
                                     esp_common
                                     esp_mm
                                     log
                                     efuse
                                     fatfs
                                     sdmmc
                                     esp_codec_dev
                                     esp_lcd_jd9165
                                     esp_lcd_touch_gt911
                                     esp_tinyusb
                       EMBED_FILES "canon.pcm")
