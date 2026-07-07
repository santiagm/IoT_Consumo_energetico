#ifndef THREAD_PACKET_H
#define THREAD_PACKET_H
#include <stdint.h>
#include "config.h"
#include "telemetry_packet.h"

#define THREAD_PACKET_MAGIC      0x5448
#define THREAD_PACKET_VERSION    1

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint16_t batch_id;
    uint16_t encrypted_len;
    uint8_t iv[AES_GCM_IV_LEN];
    uint8_t encrypted_payload[sizeof(sensor_packet_t)];
    uint8_t auth_tag[AES_GCM_TAG_LEN];
} thread_app_packet_t;

#endif
