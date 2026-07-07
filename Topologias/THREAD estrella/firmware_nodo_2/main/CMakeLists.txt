idf_component_register(
    SRCS
        "main.c"
        "crypto_helper.c"
        "thread_sender.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        nvs_flash
        esp_event
        esp_netif
        esp_timer
        esp_system
        esp_hw_support
        mbedtls
        openthread
        lwip
        driver
        esp_driver_gpio
)