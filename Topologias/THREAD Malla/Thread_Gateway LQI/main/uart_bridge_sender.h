#ifndef UART_BRIDGE_SENDER_H
#define UART_BRIDGE_SENDER_H
#include "esp_err.h"
esp_err_t uart_bridge_sender_init(void);
esp_err_t uart_bridge_send_json_line(const char *json);
#endif
