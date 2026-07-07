#ifndef TELEMETRY_PACKET_H
#define TELEMETRY_PACKET_H

#include <stdint.h>
#include "config.h"

/*
 * Muestra sintetica cruda compatible con el escenario 3.
 * ax, ay, az en LSB de acelerometro.
 * gx, gy, gz en LSB de giroscopio.
 */
typedef struct __attribute__((packed)) {
    int16_t ax;
    int16_t ay;
    int16_t az;
    int16_t gx;
    int16_t gy;
    int16_t gz;
} mpu_sample_t;

/*
 * Paquete cifrado que viaja por BLE GATT seguro + AES-128-GCM.
 * node_id permite que el BLE_gateway diferencie N1/N2/N3 en la topologia estrella.
 */
typedef struct __attribute__((packed)) {
    char node_id[4];
    char src_mac[18];
    uint32_t start_timestamp_ms;
    uint16_t batch_id;
    mpu_sample_t muestras[STREAM_SAMPLE_COUNT];
} sensor_packet_t;

#endif
