#ifndef CONFIG_H
#define CONFIG_H
#include <stdint.h>
#include "driver/gpio.h"

#define NODE_DEBUG_LOGS                 0
#define NODE_ID                         "N2"


#define STREAM_SAMPLE_COUNT             1
#define MPU_ACCEL_LSB_PER_G             16384.0f
#define MPU_GYRO_LSB_PER_DPS            131.0f

#define NODE_ENABLE_RTC_PERIODIC_WAKEUP 1
#define NODE_WAKEUP_PERIOD_SEC          5
#define RGB_LED_GPIO                    GPIO_NUM_8

#define AES_GCM_KEY_LEN                 16
#define AES_GCM_IV_LEN                  12
#define AES_GCM_TAG_LEN                 16
static const uint8_t AES_KEY[AES_GCM_KEY_LEN] = {
    'S','E','C','R','E','T','_','K','E','Y','_','1','2','3','4','5'
};

#define THREAD_NETWORK_NAME             "tesis-thread"
#define THREAD_CHANNEL                  25
#define THREAD_PANID                    0x1234
#define THREAD_UDP_PORT                 12345
#define THREAD_TX_POWER_DBM             5
#define THREAD_FORCE_ROUTER             0
#define THREAD_ATTACH_TIMEOUT_MS        60000U
#define THREAD_UDP_SEND_TIMEOUT_MS      1000U
#define THREAD_GATEWAY_IPV6_ADDR        "fdde:ad00:beef:0:39b3:96f1:8cab:cca1"

static const uint8_t THREAD_EXT_PANID[8] = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
static const uint8_t THREAD_NETWORK_KEY[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
static const uint8_t THREAD_MESH_LOCAL_PREFIX[8] = {0xFD,0xDE,0xAD,0x00,0xBE,0xEF,0x00,0x00};
static const uint8_t THREAD_PSKC[16] = {0xCA,0xFE,0xBA,0xBE,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B};

#define NODE_ID_STRING_MAX_LEN          18
static const gpio_num_t NODE_GPIOS_TO_RELEASE[] = {
    GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_3, GPIO_NUM_4,
    GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_8, GPIO_NUM_9,
    GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_14, GPIO_NUM_15,
    GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_20, GPIO_NUM_21, GPIO_NUM_22, GPIO_NUM_23
};
#define NODE_GPIOS_TO_RELEASE_COUNT (sizeof(NODE_GPIOS_TO_RELEASE)/sizeof(NODE_GPIOS_TO_RELEASE[0]))
#endif
