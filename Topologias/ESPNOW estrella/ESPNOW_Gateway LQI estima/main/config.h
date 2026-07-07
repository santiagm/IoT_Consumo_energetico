#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

#define ESPNOW_CHANNEL              8
#define STREAM_SAMPLE_COUNT         1
#define SYNTHETIC_SAMPLE_DELAY_MS   10

/*
 * UART hacia el MQTT_Gateway.
 * Conexion recomendada:
 *   ESPNOW_Gateway GPIO4 TX  -> MQTT_Gateway GPIO5 RX
 *   ESPNOW_Gateway GND       -> MQTT_Gateway GND
 */
#define UART_PORT_NUM               UART_NUM_1
#define UART_BAUDRATE               115200
#define UART_TX_GPIO                GPIO_NUM_4
#define UART_RX_GPIO                GPIO_NUM_5
#define UART_LINE_MAX               768

/*
 * MAC STA reales de los nodos permitidos en la topologia estrella.
 * Deben coincidir con la MAC STA que imprime cada firmware_nodo al iniciar.
 * N1 toma la MAC del ejemplo JSON que indicaste.
 * N2 toma la MAC usada previamente en tus pruebas.
 * N3 queda con la MAC del peer original del Escenario 3; cambiala si tu tercer nodo usa otra MAC.
 */
#define NODE1_ID                    "N1"
#define NODE1_PEER_MAC_BYTES        {0x98, 0xA3, 0x16, 0x96, 0x8A, 0x50}

#define NODE2_ID                    "N2"
#define NODE2_PEER_MAC_BYTES        {0x40, 0x4C, 0xCA, 0x4D, 0x03, 0x00}

#define NODE3_ID                    "N3"
#define NODE3_PEER_MAC_BYTES        {0x40, 0x4C, 0xCA, 0x4E, 0x3A, 0x34}

/*
 * Seguridad ESP-NOW.
 * PMK y LMK deben ser exactamente iguales en los 3 nodos y en este gateway.
 * Cada arreglo debe tener 16 bytes.
 */
#define ESPNOW_PMK_BYTES            { 'P','M','K','_','E','S','P','N','O','W','_','2','0','2','6','!' }
#define ESPNOW_LMK_BYTES            { 'L','M','K','_','N','O','D','O','_','G','A','T','E','_','0','1' }
