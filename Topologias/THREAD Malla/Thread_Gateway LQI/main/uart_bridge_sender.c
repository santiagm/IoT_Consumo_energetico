#include "uart_bridge_sender.h"

#include <stdbool.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "config.h"

static const char *TAG = "uart_tx";
static bool s_uart_ready = false;

esp_err_t uart_bridge_sender_init(void)
{
    if (s_uart_ready) {
        return ESP_OK;
    }

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

    s_uart_ready = true;
    ESP_LOGI(TAG, "UART bridge TX listo UART%d TX=%d RX=%d baud=%d",
             UART_BRIDGE_NUM, UART_BRIDGE_TX_GPIO, UART_BRIDGE_RX_GPIO, UART_BRIDGE_BAUD);
    return ESP_OK;
}

esp_err_t uart_bridge_send_json_line(const char *json)
{
    if (!s_uart_ready || json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t len = strlen(json);
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = uart_write_bytes(UART_BRIDGE_NUM, json, len);
    if (written != (int)len) {
        ESP_LOGE(TAG, "UART envio incompleto json written=%d len=%u", written, (unsigned)len);
        return ESP_FAIL;
    }

    written = uart_write_bytes(UART_BRIDGE_NUM, "\n", 1);
    if (written != 1) {
        ESP_LOGE(TAG, "UART no pudo enviar salto de linea");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "JSON enviado por UART len=%u", (unsigned)len);
    return ESP_OK;
}
