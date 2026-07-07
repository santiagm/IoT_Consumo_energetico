#include "uart_bridge_receiver.h"

#include <string.h>

#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "config.h"

static const char *TAG = "uart_rx";
static QueueHandle_t s_json_queue = NULL;

static void uart_rx_task(void *arg)
{
    (void)arg;

    uint8_t byte = 0;
    char line[UART_JSON_MAX_LEN];
    size_t pos = 0;

    while (true) {
        int len = uart_read_bytes(UART_BRIDGE_NUM, &byte, 1, pdMS_TO_TICKS(1000));
        if (len <= 0) {
            continue;
        }

        if (byte == '\r') {
            continue;
        }

        if (byte == '\n') {
            if (pos > 0) {
                line[pos] = '\0';

                uart_json_msg_t msg = {0};
                strncpy(msg.json, line, sizeof(msg.json) - 1);

                if (xQueueSend(s_json_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
                    ESP_LOGW(TAG, "Cola UART llena. Se pierde JSON: %s", line);
                } else {
                    ESP_LOGI(TAG, "JSON recibido por UART len=%u", (unsigned)pos);
                }

                pos = 0;
            }
            continue;
        }

        if (pos < sizeof(line) - 1) {
            line[pos++] = (char)byte;
        } else {
            ESP_LOGW(TAG, "Linea UART demasiado larga. Se descarta buffer parcial");
            pos = 0;
        }
    }
}

esp_err_t uart_bridge_receiver_init(QueueHandle_t queue)
{
    if (queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_json_queue = queue;

    const uart_config_t uart_config = {
        .baud_rate = UART_BRIDGE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_BRIDGE_NUM, UART_BRIDGE_BUF_SIZE, UART_BRIDGE_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_BRIDGE_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_BRIDGE_NUM, UART_BRIDGE_TX_GPIO, UART_BRIDGE_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(uart_rx_task, "uart_json_rx", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "UART bridge RX listo UART%d TX=%d RX=%d baud=%d",
             UART_BRIDGE_NUM, UART_BRIDGE_TX_GPIO, UART_BRIDGE_RX_GPIO, UART_BRIDGE_BAUD);

    return ESP_OK;
}
