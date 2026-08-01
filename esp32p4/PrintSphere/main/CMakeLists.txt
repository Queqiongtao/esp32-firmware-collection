set(PRINTSPHERE_MAIN_REQUIRES
    app_update
    esp_driver_uart
    esp_driver_usb_serial_jtag
    esp_pm
    esp_https_ota
    esp_driver_gpio
    esp_event
    esp_http_client
    esp_http_server
    mqtt
    esp_netif
    lwip
    esp_driver_jpeg
    esp_driver_ppa
    espressif__esp_new_jpeg
    espressif__esp_codec_dev
    esp-tls
    esp_wifi
    libpng
    lvgl
    nvs_flash
    joltwallet__littlefs
    jc1060p470_bsp
)

set(PRINTSPHERE_MAIN_PRIV_REQUIRES esp_adc)

idf_component_register(
    SRCS
        "app_main.cpp"
        "src/application.cpp"
        "src/audio_notifier.cpp"
        "src/bambu_status.cpp"
        "src/bambu_cloud_client.cpp"
        "src/config_store.cpp"
        "src/error_lookup.cpp"
        "include/font/dosis_20.c"
        "include/font/dosis_32.c"
        "include/font/dosis_40.c"
        "include/img/bambuicon.c"
        "include/font/mdi_30.c"
        "include/font/mdi_40.c"
        "src/pmu.cpp"
        "src/p1s_camera_client.cpp"
        "src/printer_client.cpp"
        "src/printer_state.cpp"
        "src/debug_log_buffer.cpp"
        "src/setup_portal.cpp"
        "src/serial_provisioner.cpp"
        "src/status_resolver.cpp"
        "src/time_sync.cpp"
        "src/ui.cpp"
        "src/wifi_manager.cpp"
    INCLUDE_DIRS
        "include"
    EMBED_TXTFILES
        "include/certs/bambu.cert"
        "include/certs/bambu_p2s_250626.cert"
        "include/certs/bambu_h2c_251122.cert"
        "include/certs/bambu_x2c_260425.cert"
        "include/error_lookup/error_lookup.tsv"
    REQUIRES
        ${PRINTSPHERE_MAIN_REQUIRES}
    PRIV_REQUIRES
        ${PRINTSPHERE_MAIN_PRIV_REQUIRES}
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PRINTSPHERE_RELEASE_VERSION="${PRINTSPHERE_RELEASE_VERSION}"
    ${PRINTSPHERE_HW_COMPILE_DEFINE}
)

string(FIND "${PRINTSPHERE_RELEASE_VERSION}" "-debug" _debug_pos)
string(FIND "${PRINTSPHERE_RELEASE_VERSION}" "_debug" _debug_pos2)
if(_debug_pos GREATER -1 OR _debug_pos2 GREATER -1)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PRINTSPHERE_DEBUG_BUILD=1)
endif()
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
