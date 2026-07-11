idf_component_register(SRCS "transport_wifi.c"
                       INCLUDE_DIRS "include"
                       REQUIRES frame_pipeline nvs_flash esp_wifi esp_event esp_netif esp_http_server lwip)
