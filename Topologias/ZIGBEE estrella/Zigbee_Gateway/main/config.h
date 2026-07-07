#pragma once
#include "driver/gpio.h"

#define DEVICE_LOGICAL_NAME          "tesis-zigbee-gateway"

#define ZIGBEE_PAN_ID                0x1234
#define ZIGBEE_CHANNEL               25
#define ZIGBEE_PRIMARY_CHANNEL_MASK      (1UL << ZIGBEE_CHANNEL)
#define ZIGBEE_ENDPOINT_NODE         10
#define ZIGBEE_ENDPOINT_GATEWAY      1
#define ZIGBEE_CUSTOM_CLUSTER_ID     0xFF00
#define ZIGBEE_CUSTOM_COMMAND_ID     0x01
#define ZIGBEE_PERMIT_JOIN_SECONDS   60

#define UART_BRIDGE_PORT             UART_NUM_1
#define UART_BRIDGE_TX_GPIO          GPIO_NUM_4
#define UART_BRIDGE_RX_GPIO          GPIO_NUM_5
#define UART_BRIDGE_BAUD             115200

/* Escenario estrella: Trust Center con 3 nodos autorizados.
 * IMPORTANTE: reemplaza las direcciones IEEE de N1/N2/N3 por las reales impresas
 * por cada firmware_nodoX cuando actives NODE_DEBUG_LOGS=1.
 * El orden es el mismo formato usado por el Escenario 3 original.
 */
#define AUTHORIZED_NODE_COUNT 3

#define AUTHORIZED_NODE1_ID "N1"
#define AUTHORIZED_NODE1_IEEE_ADDR {0x50,0x8A,0x96,0xFE,0xFF,0x16,0xA3,0x98}//Real:0x98,0xA3,0x16,0xFF,0xFE,0x96,0x8A,0x50
#define AUTHORIZED_NODE1_INSTALL_CODE_HEX "83FED3407A939723A5C639B26916D505C3B5"

#define AUTHORIZED_NODE2_ID "N2"
#define AUTHORIZED_NODE2_IEEE_ADDR {0x00,0x03,0x4D,0xFE,0xFF,0xCA,0x4C,0x40} //Real: 0x40,0x4C,0xCA,0xFF,0xFE,0x4E,0x3A,0x34
#define AUTHORIZED_NODE2_INSTALL_CODE_HEX "83FED3407A939723A5C639B26916D505C3B5"
#define AUTHORIZED_NODE3_ID "N3"
#define AUTHORIZED_NODE3_IEEE_ADDR {0x34,0x3A,0x4E,0xFE,0xFF,0xCA,0x4C,0x40} //Real: 0x40,0x4C,0xCA,0xFF,0xFE,0x4D,0x03,0x00
#define AUTHORIZED_NODE3_INSTALL_CODE_HEX "83FED3407A939723A5C639B26916D505C3B5"

/* Si la API Zigbee no entrega RSSI/LQI dentro del callback custom, el JSON conserva
 * los campos obligatorios con -1. No afecta decrypt_ok ni el envío UART.
 */
#define ZIGBEE_RX_RSSI_UNAVAILABLE   (-1)
#define ZIGBEE_RX_LQI_UNAVAILABLE    (-1)

/* Seguridad de aplicación AES-128-GCM sobre sensor_packet_t.
 * Debe coincidir exactamente con firmware_nodo1/2/3/main/config.h.
 * Clave de laboratorio para comparación experimental; cambiar por clave única en producción.
 */
#define APP_AES_128_GCM_KEY_BYTES {     0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,     0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF  }
