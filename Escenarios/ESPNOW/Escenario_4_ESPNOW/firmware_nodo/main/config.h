#ifndef CONFIG_H
#define CONFIG_H

#include "driver/gpio.h"

#define ESPNOW_CHANNEL          6//8
#define WIFI_TX_POWER_QDBM      20
#define I2C_SDA_GPIO            6
#define I2C_SCL_GPIO            7
#define MPU_I2C_ADDR            0x69
#define MPU_INT_GPIO            GPIO_NUM_4

#define MOTION_THRESHOLD        12
#define MOTION_DURATION         2

#define MPU_SAMPLE_RATE_HZ      100

#define RECORD_TIME_MS          1000
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