#pragma once

#define NODE_ID                      "N1"
#define DEVICE_LOGICAL_NAME          "tesis-zigbee-nodo1"

#define ZIGBEE_PAN_ID                0x1234
#define ZIGBEE_CHANNEL               25
#define ZIGBEE_PRIMARY_CHANNEL_MASK      (1UL << ZIGBEE_CHANNEL)
#define ZIGBEE_ENDPOINT_NODE         10
#define ZIGBEE_ENDPOINT_GATEWAY      1
#define ZIGBEE_CUSTOM_CLUSTER_ID     0xFF00
#define ZIGBEE_CUSTOM_COMMAND_ID     0x01
#define ZIGBEE_TX_POWER_DBM          5

/* Optimización de tiempos activos del nodo Zigbee: valores conservados del Escenario 3. */
#define NODE_DEBUG_LOGS              1
#define ZIGBEE_JOIN_POLL_MS          5
#define POST_ZIGBEE_TX_DELAY_MS      10

#define NODE_SLEEP_TIME_US           (5ULL * 1000000ULL)

#ifndef ZIGBEE_ED_KEEPALIVE_MS
#define ZIGBEE_ED_KEEPALIVE_MS       3000
#endif

/* 128-bit Install Code + 16-bit CRC = 18 bytes.
 * Mantener idéntico en el Gateway para el nodo correspondiente.
 * Valor de laboratorio. Cambiar por un código único en producción si necesitas separar credenciales.
 */
#define ZIGBEE_INSTALL_CODE_HEX      "83FED3407A939723A5C639B26916D505C3B5"

/* Seguridad de aplicación AES-128-GCM sobre sensor_packet_t.
 * Debe coincidir exactamente con Zigbee_Gateway/main/config.h.
 * Clave de laboratorio para comparación experimental; cambiar por clave única en producción.
 */
#define APP_AES_128_GCM_KEY_BYTES {     0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,     0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF  }
