#ifndef ESPNOW_HANDLER_H
#define ESPNOW_HANDLER_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t espnow_handler_init(void);
esp_err_t espnow_handler_send(const uint8_t *data, size_t len);

#endif
