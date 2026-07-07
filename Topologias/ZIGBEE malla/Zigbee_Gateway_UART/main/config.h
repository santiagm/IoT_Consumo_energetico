#pragma once
#include "driver/gpio.h"

#define ZIGBEE_PAN_ID                0x1234
#define ZIGBEE_CHANNEL               25
#define ZIGBEE_PRIMARY_CHANNEL_MASK      (1UL << ZIGBEE_CHANNEL)
#define ZIGBEE_ENDPOINT_NODE         10
#define ZIGBEE_ENDPOINT_GATEWAY      1
#define ZIGBEE_CUSTOM_CLUSTER_ID     0xFF00
#define ZIGBEE_CUSTOM_COMMAND_ID     0x01
#define ZIGBEE_PERMIT_JOIN_SECONDS   180

#define UART_BRIDGE_PORT             UART_NUM_1
#define UART_BRIDGE_TX_GPIO          GPIO_NUM_4
#define UART_BRIDGE_RX_GPIO          GPIO_NUM_5
#define UART_BRIDGE_BAUD             115200

/* Install Codes autorizados para la malla.
 * MUY IMPORTANTE: reemplazar cada IEEE por el IEEE802154 real impreso por cada placa.
 * El orden de bytes debe coincidir con la API local; si el join falla, revisar el orden.
 */
#define AUTHORIZED_N1_IEEE_ADDR {0x98,0xA3,0x16,0xFF,0xFE,0x96,0x8A,0x50} //0x40,0x4C,0xCA,0xFF,0xFE,0x4D,0x03,0x00 //
#define AUTHORIZED_N2_IEEE_ADDR {0x40,0x4C,0xCA,0xFF,0xFE,0x4E,0x3A,0x34}//0x98,0xA3,0x16,0xFF,0xFE,0x96,0x8A,0x50
#define AUTHORIZED_N3_IEEE_ADDR {0x40,0x4C,0xCA,0xFF,0xFE,0x4D,0x03,0x00} //0x98,0xA3,0x16,0xFF,0xFE,0x96,0x8A,0x50

#define AUTHORIZED_N1_INSTALL_CODE_HEX "83FED3407A939723A5C639B26916D505C3B5" 
#define AUTHORIZED_N2_INSTALL_CODE_HEX "A4FCD2318B84A632B6D748C37E25E614851C"
#define AUTHORIZED_N3_INSTALL_CODE_HEX "B5EDD3429C95B743C7E859D48F36F7256062"

/* Seguridad de aplicación AES-128-GCM sobre sensor_packet_t.
 * Debe coincidir exactamente con firmware_nodo/main/config.h.
 * Clave de laboratorio para comparación experimental; cambiar por clave única en producción.
 */
#define APP_AES_128_GCM_KEY_BYTES { \
    0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, \
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF  \
}
