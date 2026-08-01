idf_component_register(
    SRCS "block_physics.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_hw_support esp_lcd esp_timer freertos
)
