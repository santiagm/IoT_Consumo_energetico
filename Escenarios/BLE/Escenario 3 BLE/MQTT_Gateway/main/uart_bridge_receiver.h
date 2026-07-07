#ifndef UART_BRIDGE_RECEIVER_H
#define UART_BRIDGE_RECEIVER_H
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "config.h"

typedef struct {
    char json[UART_JSON_MAX_LEN];
} uart_json_msg_t;

esp_err_t uart_bridge_receiver_init(QueueHandle_t queue);

#endif
