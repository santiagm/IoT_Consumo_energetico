idf_component_register(
    SRCS "ble_scanner.c" "crypto_helper.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash mbedtls bt esp_timer mqtt cjson esp_driver_uart
)
