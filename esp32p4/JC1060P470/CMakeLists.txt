cmake_minimum_required(VERSION 3.22)

# esp_wifi_remote selects version-specific Kconfig fragments by $ESP_IDF_VERSION.
# Clamp 6.0.x to 6.0 so the component loads its matching fragment on IDF v6.0.1.
if(DEFINED ENV{ESP_IDF_VERSION} AND "$ENV{ESP_IDF_VERSION}" MATCHES "^6\\.0(\\..*)?$")
    set(ENV{ESP_IDF_VERSION} "6.0")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(JC1060P470)
