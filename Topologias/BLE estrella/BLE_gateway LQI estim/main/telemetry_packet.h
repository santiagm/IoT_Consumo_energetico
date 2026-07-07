#ifndef TELEMETRY_PACKET_H
#define TELEMETRY_PACKET_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

/*
 * Estructura común entre nodos BLE y BLE_gateway.
 * Se mantiene STREAM_SAMPLE_COUNT=1 para conservar el escenario 3 sin MPU:
 * datos sintéticos + BLE seguro + AES-128-GCM.
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
    char node_id[4];        /* "N1", "N2" o "N3" */
    char src_mac[18];
    uint32_t start_timestamp_ms;
    uint16_t batch_id;
    mpu_sample_t muestras[STREAM_SAMPLE_COUNT];
} sensor_packet_t;

/*
 * Metadatos que solo existen en el BLE_gateway.
 * RSSI y flags no viajan cifrados desde el nodo; los agrega el gateway.
 */
typedef struct {
    sensor_packet_t packet;
    int8_t rssi;
    bool duplicate;
    bool decrypt_ok;
} gateway_packet_t;

#endif
