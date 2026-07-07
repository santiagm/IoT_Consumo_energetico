#pragma once

#define NODE_ID                      "N3"

#define ZIGBEE_PAN_ID                0x1234
#define ZIGBEE_CHANNEL               25
#define ZIGBEE_PRIMARY_CHANNEL_MASK      (1UL << ZIGBEE_CHANNEL)
#define ZIGBEE_ENDPOINT_NODE         10 
#define ZIGBEE_ENDPOINT_GATEWAY      1
#define ZIGBEE_CUSTOM_CLUSTER_ID     0xFF00
#define ZIGBEE_CUSTOM_COMMAND_ID     0x01
#define ZIGBEE_TX_POWER_DBM          5

/* Optimización de tiempos activos del router Zigbee */
#define NODE_DEBUG_LOGS              1
#define ZIGBEE_JOIN_POLL_MS          5
#define POST_ZIGBEE_TX_DELAY_MS      10

/* Diagnóstico Mesh del Router N3 */
#define ROUTER_MAX_CHILDREN          10
#define ROUTER_EXPECTED_SED_CHILDREN 2
#define ROUTER_PERMIT_JOIN_SECONDS   180
#define ROUTER_NETWORK_TABLE_LOG_MS  20000

/* ROUTER: No deep sleep. N3 permanece siempre encendido. */
#define NODE_SLEEP_TIME_US           0

#ifndef ZIGBEE_ED_KEEPALIVE_MS
#define ZIGBEE_ED_KEEPALIVE_MS         3000
#endif

/* 128-bit Install Code + 16-bit CRC = 18 bytes.
 * DIFERENTE para N3 (Router). Reemplazar por un código único en producción.
 * Formato: 16 bytes de código + 2 bytes de CRC (36 caracteres hex).
 */
#define ZIGBEE_INSTALL_CODE_HEX "B5EDD3429C95B743C7E859D48F36F7256062"

/* Seguridad de aplicación AES-128-GCM sobre sensor_packet_t.
 * Debe coincidir exactamente con todos los nodos y el gateway.
 * Clave de laboratorio para comparación experimental; cambiar por clave única en producción.
 */
#define APP_AES_128_GCM_KEY_BYTES { \
    0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, \
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF  \
}
