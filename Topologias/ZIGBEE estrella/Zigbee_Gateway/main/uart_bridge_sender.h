#ifndef UART_BRIDGE_SENDER_H
#define UART_BRIDGE_SENDER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UART_BRIDGE_PORT              UART_NUM_1
#define UART_BRIDGE_TX_GPIO           GPIO_NUM_4
#define UART_BRIDGE_RX_GPIO           GPIO_NUM_5
#define UART_BRIDGE_BAUDRATE          115200
#define UART_BRIDGE_TX_BUF_SIZE       2048
#define UART_BRIDGE_RX_BUF_SIZE       256

/*
 * Tamaño máximo del JSON enviado desde el Zigbee Gateway
 * hacia el MQTT Gateway.
 *
 * Lo dejamos en 1024 para evitar el error previo de JSON_ERR
 * por buffer pequeño de 512 bytes.
 */
#define UART_BRIDGE_MAX_JSON_LEN      1024

esp_err_t uart_bridge_sender_init(void);
esp_err_t uart_bridge_send_json(const char *json);

#ifdef __cplusplus
}
#endif

#endif