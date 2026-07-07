#pragma once

#define NODE_ID                      "N2"

#define ZIGBEE_PAN_ID                0x1234
#define ZIGBEE_CHANNEL               25
#define ZIGBEE_PRIMARY_CHANNEL_MASK      (1UL << ZIGBEE_CHANNEL)
#define ZIGBEE_ENDPOINT_NODE         10
#define ZIGBEE_ENDPOINT_GATEWAY      1
#define ZIGBEE_CUSTOM_CLUSTER_ID     0xFF00
#define ZIGBEE_CUSTOM_COMMAND_ID     0x01
#define ZIGBEE_TX_POWER_DBM          5

/* Debug inicial para ver el IEEE real de N2.
 * Para medición de consumo dejar en 0 después de copiar el IEEE al Gateway.
 */
#define NODE_DEBUG_LOGS              1

/* No dejar en 5 ms: puede saturar la tarea main mientras espera join y disparar Task WDT. */
#define ZIGBEE_JOIN_POLL_MS          50
#define POST_ZIGBEE_TX_DELAY_MS      100

#define NODE_SLEEP_TIME_US           (5ULL * 1000000ULL)

#ifndef ZIGBEE_ED_KEEPALIVE_MS
#define ZIGBEE_ED_KEEPALIVE_MS       3000
#endif

/* N2: 128-bit Install Code + 16-bit CRC = 18 bytes = 36 caracteres hex.
 * Debe coincidir exactamente con AUTHORIZED_N2_INSTALL_CODE_HEX del Gateway.
 */
#define ZIGBEE_INSTALL_CODE_HEX "A4FCD2318B84A632B6D748C37E25E614851C"

/* Seguridad de aplicación AES-128-GCM sobre sensor_packet_t.
 * Debe coincidir exactamente con N1, N3 y Zigbee_Gateway_UART.
 */
#define APP_AES_128_GCM_KEY_BYTES { \
    0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, \
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF  \
}
