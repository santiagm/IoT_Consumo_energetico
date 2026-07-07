#ifndef TELEMETRY_PACKET_H
#define TELEMETRY_PACKET_H

#include <stdint.h>
#include "config.h"

/*
 * Estructura común entre nodo BLE y gateway BLE.
 *
 * Se incluye src_mac dentro del paquete cifrado para mantener en ThingsBoard
 * el mismo campo usado en ESP-NOW, aunque el transporte ahora sea BLE.
 *
 * Tamaño con STREAM_SAMPLE_COUNT=10:
 * 18 bytes src_mac + 4 timestamp + 2 batch + 10*(6*2 bytes) = 144 bytes.
 */
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
    uint32_t start_timestamp_ms;
    uint16_t batch_id;
    mpu_sample_t muestras[STREAM_SAMPLE_COUNT];
} sensor_packet_t;

#endif
