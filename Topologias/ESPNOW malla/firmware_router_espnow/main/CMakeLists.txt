idf_component_register(
    SRCS
        "main.c"
        "espnow_handler.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        lwip
        esp_timer
        esp_driver_gpio
        mbedtls
)
