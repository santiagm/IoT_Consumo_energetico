#ifndef TELEMETRY_JSON_H
#define TELEMETRY_JSON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "telemetry_packet.h"

esp_err_t telemetry_json_build(const sensor_packet_t *pkt,
                               uint32_t ts_gateway_ms,
                               bool duplicate,
                               bool decrypt_ok,
                               int rssi,
                               int lqi,
                               char *out,
                               size_t out_len);

#endif
