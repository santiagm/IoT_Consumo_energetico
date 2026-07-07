#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "telemetry_packet.h"

esp_err_t zigbee_sender_start(void);
bool zigbee_sender_is_joined(void);
esp_err_t zigbee_sender_send_packet(const secure_zigbee_packet_t *packet);
void zigbee_sender_deinit_before_sleep(void);