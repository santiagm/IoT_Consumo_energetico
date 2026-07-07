#ifndef SECURITY_PAYLOAD_H
#define SECURITY_PAYLOAD_H

#include <stdint.h>
#include "config.h"

#define APP_AES_GCM_KEY_BYTES { 'A','E','S','G','C','M','_','E','S','P','N','O','W','2','6','!' }
#define ESPNOW_APP_SEC_MAGIC 0x474E5345UL
#define ESPNOW_APP_SEC_VERSION 1U
#define AES_GCM_IV_LEN 12U
#define AES_GCM_TAG_LEN 16U

typedef struct __attribute__((packed)) {
    int16_t ax, ay, az, gx, gy, gz;
} mpu_sample_t;

typedef struct __attribute__((packed)) {
    char node_id[4];
    char src_mac[18];
    uint32_t packet_id;
    uint16_t batch_id;
    uint32_t node_tx_timestamp_ms;
    mpu_sample_t muestras[STREAM_SAMPLE_COUNT];
} sensor_packet_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint16_t batch_id;
    uint16_t encrypted_len;
    uint8_t iv[AES_GCM_IV_LEN];
} secure_packet_aad_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint16_t batch_id;
    uint16_t encrypted_len;
    uint8_t iv[AES_GCM_IV_LEN];
    uint8_t encrypted_payload[sizeof(sensor_packet_t)];
    uint8_t auth_tag[AES_GCM_TAG_LEN];
} secure_espnow_packet_t;

#endif
