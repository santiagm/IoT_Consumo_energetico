#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "telemetry_packet.h"

esp_err_t app_crypto_decrypt_sensor_packet(const secure_zigbee_packet_t *secure_pkt,
                                           sensor_packet_t *plain);
