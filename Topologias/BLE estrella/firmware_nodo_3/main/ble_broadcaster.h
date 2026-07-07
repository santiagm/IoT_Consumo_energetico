#ifndef BLE_BROADCASTER_H
#define BLE_BROADCASTER_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t ble_broadcaster_init(void);
esp_err_t ble_broadcaster_send_packet(uint16_t batch_id,
                                      const uint8_t *encrypted_data,
                                      size_t encrypted_len,
                                      const uint8_t *auth_tag);
void ble_broadcaster_stop(void);

#endif
