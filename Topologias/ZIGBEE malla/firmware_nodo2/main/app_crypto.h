#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "telemetry_packet.h"

esp_err_t app_crypto_encrypt_sensor_packet(const sensor_packet_t *plain,
                                           secure_zigbee_packet_t *secure_pkt);
