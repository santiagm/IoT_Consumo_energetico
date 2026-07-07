#ifndef CONFIG_H
#define CONFIG_H
#include "driver/uart.h"
#define ESPNOW_CHANNEL 8
#define STREAM_SAMPLE_COUNT 1
#define STREAM_SAMPLE_DELAY_MS 10
#define ROUTER_MAC_BYTES {0x40,0x4C,0xCA,0x4D,0x03,0x00}//  0x40,0x4C,0xCA,0x4D,0x03,0x00   / 0x98,0xA3,0x16,0x96,0x8A,0x50
#define ESPNOW_PMK_BYTES { 'P','M','K','_','E','S','P','N','O','W','_','2','0','2','6','!' }
#define ESPNOW_LMK_BYTES { 'L','M','K','_','N','O','D','O','_','G','A','T','E','_','0','1' }
/* UART hacia MQTT_Gateway. Ajustar pines al cableado real. */
#define UART_PORT_NUM UART_NUM_1
#define UART_BAUDRATE 115200
#define UART_TX_GPIO 4
#define UART_RX_GPIO 5
#endif
