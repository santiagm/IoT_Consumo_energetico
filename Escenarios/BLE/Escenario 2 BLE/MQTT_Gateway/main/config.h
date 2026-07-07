#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include "driver/gpio.h"

#define GATEWAY_ENABLE_MQTT       1
#define GATEWAY_DEBUG_LOGS        1

/* Cambia aquí la red WiFi del ESP32-C6 que publica a ThingsBoard. */
#define WIFI_SSID                 "diqueerespobre"
#define WIFI_PASS                 "soypobre"

#define THINGSBOARD_HOST          "mqtt.thingsboard.cloud"
#define THINGSBOARD_PORT          1883
#define THINGSBOARD_TOKEN         "Wj2rLAd4xKoOlPpuN0u7"
#define THINGSBOARD_TOPIC         "v1/devices/me/telemetry"
#define MQTT_CLIENT_ID            "gateway-esp32c6-mqtt-uart"

#define UART_BRIDGE_NUM           UART_NUM_1
#define UART_BRIDGE_TX_GPIO       GPIO_NUM_4
#define UART_BRIDGE_RX_GPIO       GPIO_NUM_5
#define UART_BRIDGE_BAUD          115200
#define UART_BRIDGE_BUF_SIZE      1024
#define UART_JSON_MAX_LEN         512
#define UART_QUEUE_LENGTH         16

#endif
