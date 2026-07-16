set(main_sources
    "JC1060P470_main.c"
    "app_peripherals.c"
    "board_i2c.c"
    "display_board.c"
    "display_font.c"
    "display_panel.c"
    "display_lvgl.c"
    "display_touch.c"
    "lvgl_demo_font_patch.c"
    "lvgl_demo_ui.c"
    "test_usb_role.c"
)

set(main_private_requires
    esp_driver_gpio
    esp_driver_ledc
    esp_driver_i2c
    esp_hw_support
    esp_timer
    esp_lcd
    esp_common
    esp_mm
    nvs_flash
    log
    efuse
    spiffs
    esp_lcd_jd9165
    esp_lcd_touch_gt911
    # IDF minimal-build dependency discovery runs before sdkconfig is loaded.
    # Keep optional built-in components discoverable; their sources are still
    # selected below and unused libraries are removed by the linker.
    esp_driver_sdmmc
    fatfs
    sdmmc
    esp_driver_i2s
    esp_adc
    esp_driver_jpeg
    esp_driver_ppa
    esp_event
    esp_netif
    esp_wifi
    bt
)

set(main_private_include_dirs)

if(CONFIG_EXAMPLE_ENABLE_TEST_SDCARD)
    list(APPEND main_sources "test_sdcard.c")
endif()

if(CONFIG_EXAMPLE_ENABLE_TEST_RTC)
    list(APPEND main_sources "test_rtc.c")
endif()

if(CONFIG_EXAMPLE_ENABLE_TEST_AUDIO)
    list(APPEND main_sources "test_audio.c")
endif()

if(CONFIG_EXAMPLE_ENABLE_TEST_BATTERY)
    list(APPEND main_sources "test_battery.c")
endif()

if(CONFIG_EXAMPLE_ENABLE_TEST_USB)
    list(APPEND main_sources "test_usb.c")
endif()

if(CONFIG_EXAMPLE_ENABLE_TEST_UVC)
    list(APPEND main_sources "test_uvc.c")
endif()

if(CONFIG_EXAMPLE_ENABLE_TEST_WIRELESS)
    list(APPEND main_sources "test_wireless.c")
    list(APPEND main_private_include_dirs
        "../managed_components/espressif__esp_hosted/host"
        "../managed_components/espressif__esp_hosted/host/api/include"
    )
endif()

idf_component_register(
    SRCS ${main_sources}
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS ${main_private_include_dirs}
    REQUIRES lvgl
    PRIV_REQUIRES ${main_private_requires}
)

# Managed dependencies are selected by idf_component.yml after sdkconfig is
# available. Link only the managed components present in that resolved graph.
idf_component_optional_requires(PRIVATE
    esp_codec_dev
    esp_tinyusb
    espressif__usb
    espressif__usb_host_uvc
    espressif__esp_hosted
)

if(CONFIG_EXAMPLE_ENABLE_TEST_AUDIO)
    target_add_binary_data(${COMPONENT_LIB} "canon.pcm" BINARY)
endif()

spiffs_create_partition_image(font ../assets/fonts FLASH_IN_PROJECT)
