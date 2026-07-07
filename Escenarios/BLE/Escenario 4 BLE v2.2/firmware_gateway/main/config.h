#pragma once

#include <stdint.h>

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

// ============================================================
// MODO DEL GATEWAY
// ============================================================
// 1 = Recibe BLE y publica directo a ThingsBoard por WiFi/MQTT.
// 0 = Solo BLE, útil para pruebas de comunicación/consumo sin WiFi.
#define GATEWAY_ENABLE_MQTT     0
#define GATEWAY_DEBUG_LOGS      1

// ============================================================
// WIFI Y THINGSBOARD
// ============================================================
#define WIFI_SSID               "diqueerespobre"
#define WIFI_PASS               "soypobre"

#define THINGSBOARD_HOST        "mqtt.thingsboard.cloud"
#define THINGSBOARD_PORT        1883
#define THINGSBOARD_TOKEN       "Wj2rLAd4xKoOlPpuN0u7"
#define THINGSBOARD_TOPIC       "v1/devices/me/telemetry"
#define MQTT_CLIENT_ID          "gateway-esp32c6-ble"

// ============================================================
// TELEMETRÍA: debe coincidir exactamente con el nodo
// ============================================================
#define STREAM_SAMPLE_COUNT     1

// ============================================================
// SEGURIDAD AES-128-GCM: debe coincidir exactamente con el nodo
// ============================================================
static const uint8_t AES_KEY[16] = {
    'S', 'E', 'C', 'R', 'E', 'T', '_', 'K',
    'E', 'Y', '_', '1', '2', '3', '4', '5'
};

// ============================================================
// BLE EXTENDED SCANNER
// ============================================================
#define BLE_COMPANY_ID          0xFFFF

// Unidad BLE: 0.625 ms. 0x00A0 = 100 ms, 0x0050 = 50 ms.
// Este 50% duty-cycle prioriza no perder paquetes. Para escenarios
// energéticos puedes bajar BLE_SCAN_WINDOW más adelante.
#define BLE_SCAN_INTERVAL       0x0030
#define BLE_SCAN_WINDOW         0x0030

// Cola entre BLE y MQTT
#define GATEWAY_PACKET_QUEUE_LEN 20


// ============================================================
// UART ENTRE GATEWAYS
// Gateway BLE: TX GPIO4 -> RX GPIO5 del Gateway MQTT.
// Gateway MQTT: TX GPIO4 -> RX GPIO5 del Gateway BLE.
// GND comun obligatorio.
// ============================================================
#define UART_BRIDGE_PORT        1
#define UART_BRIDGE_TX_GPIO     4
#define UART_BRIDGE_RX_GPIO     5
#define UART_BRIDGE_BAUD_RATE   115200
#define UART_BRIDGE_RX_BUF_SIZE 256
#define UART_BRIDGE_TX_BUF_SIZE 256

// WiFi power save:
// 0 = mayor estabilidad/menor latencia MQTT, más consumo.
// 1 = menor consumo, puede aumentar latencia y pérdidas si la red es inestable.
#define GATEWAY_WIFI_POWER_SAVE 0
