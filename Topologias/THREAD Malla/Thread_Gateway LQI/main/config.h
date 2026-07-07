#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include "driver/gpio.h"

#define GATEWAY_DEBUG_LOGS        1

#define STREAM_SAMPLE_COUNT       1
#define MPU_ACCEL_LSB_PER_G       16384.0f
#define MPU_GYRO_LSB_PER_DPS      131.0f

#define AES_GCM_KEY_LEN           16
#define AES_GCM_IV_LEN            12
#define AES_GCM_TAG_LEN           16
static const uint8_t AES_KEY[AES_GCM_KEY_LEN] = {
    'S','E','C','R','E','T','_','K','E','Y','_','1','2','3','4','5'
};

#define THREAD_NETWORK_NAME       "tesis-thread"
#define THREAD_CHANNEL            25
#define THREAD_PANID              0x1234
#define THREAD_UDP_PORT           12345
#define THREAD_TX_POWER_DBM       0
#define THREAD_ATTACH_TIMEOUT_MS  60000U

/* Mantén esta dirección igual a la que usa el nodo si está fija allí. */
#define THREAD_GATEWAY_IPV6_ADDR  "fdde:ad00:beef:0:c6b6:934b:ca23:4c93"

static const uint8_t THREAD_EXT_PANID[8] = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
static const uint8_t THREAD_NETWORK_KEY[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
static const uint8_t THREAD_MESH_LOCAL_PREFIX[8] = {0xFD,0xDE,0xAD,0x00,0xBE,0xEF,0x00,0x00};
static const uint8_t THREAD_PSKC[16] = {0xCA,0xFE,0xBA,0xBE,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B};

#define UART_BRIDGE_NUM           UART_NUM_1
#define UART_BRIDGE_TX_GPIO       GPIO_NUM_4
#define UART_BRIDGE_RX_GPIO       GPIO_NUM_5
#define UART_BRIDGE_BAUD          115200
#define UART_BRIDGE_BUF_SIZE      1024

#endif
