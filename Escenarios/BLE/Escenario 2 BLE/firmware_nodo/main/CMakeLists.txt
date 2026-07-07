idf_component_register(
    SRCS "ble_broadcaster.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_event nvs_flash bt esp_timer esp_hw_support
)
