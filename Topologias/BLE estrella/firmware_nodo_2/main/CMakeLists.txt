idf_component_register(
    SRCS "ble_broadcaster.c" "crypto_helper.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_event nvs_flash mbedtls bt esp_timer esp_hw_support
)
