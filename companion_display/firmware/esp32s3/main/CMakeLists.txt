idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES subscreen_display frame_pipeline transport_wifi
                                nvs_flash esp_event esp_netif spi_flash)
