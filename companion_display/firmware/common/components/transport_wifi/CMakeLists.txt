idf_component_register(SRCS "transport_wifi.c" "companion_control.c" "companion_udp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES frame_pipeline touch_input nvs_flash esp_wifi esp_event esp_netif esp_http_server lwip)
