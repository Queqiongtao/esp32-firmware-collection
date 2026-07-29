cmake_minimum_required(VERSION 3.22)

set(IDF_TARGET "esp32p4")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(PRINTSPHERE_RELEASE_VERSION "v1.6.2-p4.1")
set(PROJECT_VER "${PRINTSPHERE_RELEASE_VERSION}")
set(PRINTSPHERE_HW_VARIANT_LABEL "JC1060P470")
set(PRINTSPHERE_HW_COMPILE_DEFINE "PRINTSPHERE_HW_VARIANT_JC1060P470=1")
set(PRINTSPHERE_RELEASE_ROOT "${CMAKE_SOURCE_DIR}/release/esp32p4-jc1060p470")
set(PRINTSPHERE_RELEASE_VERSION_SUFFIX
    "${PRINTSPHERE_RELEASE_VERSION}-esp32p4-jc1060p470")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(printsphere_p4)

# IDF 6.0.1's private periph_ctrl.h uses bool without including stdbool.h.
# Force the standard definition only for the built-in SPI driver that reaches
# the header before another include supplies it.
idf_component_get_property(ESP_DRIVER_SPI_LIB esp_driver_spi COMPONENT_LIB)
if(ESP_DRIVER_SPI_LIB)
    target_compile_options(${ESP_DRIVER_SPI_LIB} PRIVATE "-include" "stdbool.h")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(RELEASE_INITIAL_FLASH_DIR "${PRINTSPHERE_RELEASE_ROOT}/initial")
set(RELEASE_INITIAL_FLASH_IMAGE "${RELEASE_INITIAL_FLASH_DIR}/printsphere_full.bin")
set(RELEASE_OTA_DIR "${PRINTSPHERE_RELEASE_ROOT}/ota")
set(RELEASE_OTA_IMAGE "${RELEASE_OTA_DIR}/printsphere_ota.bin")

add_custom_target(
    release_initial_flash
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RELEASE_INITIAL_FLASH_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RELEASE_OTA_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/package_initial_flash.py"
            --build-dir "${CMAKE_BINARY_DIR}"
            --release-root "${PRINTSPHERE_RELEASE_ROOT}"
            --output "${RELEASE_INITIAL_FLASH_IMAGE}"
            --ota-output "${RELEASE_OTA_IMAGE}"
            --version "${PRINTSPHERE_RELEASE_VERSION_SUFFIX}"
    DEPENDS app bootloader "${CMAKE_BINARY_DIR}/flasher_args.json"
    COMMENT "Packaging ${PRINTSPHERE_HW_VARIANT_LABEL} release images"
    VERBATIM
)
