#ifndef CONFIG_H
#define CONFIG_H

#include "driver/gpio.h"

#define NODE_ID                 "N3"
#define ESPNOW_CHANNEL          8

#define STREAM_SAMPLE_COUNT     1
#define STREAM_SAMPLE_DELAY_MS  10

#define ESPNOW_MAX_DATA_LEN     250

#define GATEWAY_MAC_BYTES       {0x40, 0x4C, 0xCA, 0x55, 0xAB, 0x10}

/*
 * Seguridad ESP-NOW.
 * PMK y LMK deben ser exactamente iguales en nodo y gateway.
 * Cada arreglo debe tener 16 bytes.
 */
#define ESPNOW_PMK_BYTES        { 'P','M','K','_','E','S','P','N','O','W','_','2','0','2','6','!' }
#define ESPNOW_LMK_BYTES        { 'L','M','K','_','N','O','D','O','_','G','A','T','E','_','0','1' }

#endif