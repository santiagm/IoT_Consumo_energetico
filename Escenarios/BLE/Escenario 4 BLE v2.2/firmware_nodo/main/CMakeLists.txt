idf_component_register(
    SRCS "ble_broadcaster.c" "crypto_helper.c" "main.c" "mpu6050.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp_driver_gpio esp_event nvs_flash mbedtls bt esp_timer esp_hw_support
)
