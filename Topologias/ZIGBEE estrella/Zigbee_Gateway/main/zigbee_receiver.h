#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t rx;
    uint32_t uart;
    uint32_t dup;
    uint32_t inv;
    uint32_t dec_err;
    uint32_t uart_err;
} zigbee_gateway_metrics_t;

esp_err_t zigbee_receiver_start(void);
const zigbee_gateway_metrics_t *zigbee_receiver_metrics(void);