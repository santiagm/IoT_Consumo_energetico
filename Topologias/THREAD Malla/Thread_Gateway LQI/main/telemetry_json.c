#include "telemetry_json.h"

#include <stdio.h>
#include <stdbool.h>
#include "config.h"

esp_err_t telemetry_json_build(const sensor_packet_t *pkt,
                               uint32_t ts_gateway_ms,
                               bool duplicate,
                               bool decrypt_ok,
                               int rssi,
                               int lqi,
                               char *out,
                               size_t out_len)
{
    if (!pkt || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const mpu_sample_t *s = &pkt->muestras[0];

    float ax = s->ax / MPU_ACCEL_LSB_PER_G;
    float ay = s->ay / MPU_ACCEL_LSB_PER_G;
    float az = s->az / MPU_ACCEL_LSB_PER_G;
    float gx = s->gx / MPU_GYRO_LSB_PER_DPS;
    float gy = s->gy / MPU_GYRO_LSB_PER_DPS;
    float gz = s->gz / MPU_GYRO_LSB_PER_DPS;

    uint32_t latency_ms = 0;
    if (ts_gateway_ms >= pkt->start_timestamp_ms) {
        latency_ms = ts_gateway_ms - pkt->start_timestamp_ms;
    }

    /*
     * Nota: latency_ms es aproximado si el nodo y el gateway no comparten
     * una referencia temporal sincronizada. Sirve como métrica relativa para
     * las pruebas del escenario.
     * RSSI/LQI se dejan como -127/-1 si no están disponibles en el callback UDP.
     */
    int n = snprintf(out, out_len,
        "{\"node_id\":\"%s\","
        "\"src_mac\":\"%s\","
        "\"packet_id\":%u,"
        "\"batch_id\":%u,"
        "\"ts_gateway_ms\":%lu,"
        "\"node_tx_timestamp_ms\":%lu,"
        "\"latency_ms\":%lu,"
        "\"rssi\":%d,"
        "\"lqi\":%d,"
        "\"duplicate\":%s,"
        "\"decrypt_ok\":%s,"
        "\"values\":{"
        "\"synthetic_ax\":%.5f,"
        "\"synthetic_ay\":%.5f,"
        "\"synthetic_az\":%.5f,"
        "\"synthetic_gx\":%.5f,"
        "\"synthetic_gy\":%.5f,"
        "\"synthetic_gz\":%.5f}}",
        pkt->node_id,
        pkt->src_mac,
        pkt->batch_id,
        pkt->batch_id,
        (unsigned long)ts_gateway_ms,
        (unsigned long)pkt->start_timestamp_ms,
        (unsigned long)latency_ms,
        rssi,
        lqi,
        duplicate ? "true" : "false",
        decrypt_ok ? "true" : "false",
        ax, ay, az, gx, gy, gz);

    return (n > 0 && (size_t)n < out_len) ? ESP_OK : ESP_ERR_NO_MEM;
}
