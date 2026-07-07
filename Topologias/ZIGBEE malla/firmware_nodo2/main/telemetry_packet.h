#pragma once
#include <stdint.h>
#include <stddef.h>

#define STREAM_SAMPLE_COUNT          1

#define SENSOR_PACKET_VERSION        1
#define SECURE_ZB_MAGIC              0xA5C6BEE4UL
#define SECURE_ZB_VERSION            1
#define SECURE_ZB_IV_LEN             12
#define SECURE_ZB_TAG_LEN            16

/* Paquete lógico obligatorio antes del cifrado de aplicación. */
typedef struct __attribute__((packed)) {
    int16_t ax;
    int16_t ay;
    int16_t az;
    int16_t gx;
    int16_t gy;
    int16_t gz;
} mpu_sample_t;

typedef struct __attribute__((packed)) {
    char src_mac[18];
    char node_id[4];                 /* "N1", "N2", "N3" */
    uint32_t packet_id;              /* Contador de paquetes por nodo */
    uint32_t start_timestamp_ms;
    uint16_t batch_id;
    mpu_sample_t muestras[STREAM_SAMPLE_COUNT];
} sensor_packet_t;

/* Cabecera autenticada como AAD de AES-GCM. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint16_t batch_id;
    uint16_t encrypted_len;
} secure_zigbee_aad_t;

/* Payload transportado por el cluster custom Zigbee. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint16_t batch_id;
    uint16_t encrypted_len;
    uint8_t iv[SECURE_ZB_IV_LEN];
    uint8_t encrypted_payload[sizeof(sensor_packet_t)];
    uint8_t auth_tag[SECURE_ZB_TAG_LEN];
} secure_zigbee_packet_t;

#define SENSOR_PACKET_SIZE           ((uint16_t)sizeof(sensor_packet_t))
#define SECURE_ZIGBEE_PACKET_SIZE    ((uint16_t)sizeof(secure_zigbee_packet_t))
