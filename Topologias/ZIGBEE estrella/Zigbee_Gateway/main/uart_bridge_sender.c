#include <string.h>

#include "esp_log.h"
#include "esp_err.h"

#include "driver/uart.h"
#include "driver/gpio.h"

#include "uart_bridge_sender.h"

static const char *TAG = "UART_BRIDGE";

esp_err_t uart_bridge_sender_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = UART_BRIDGE_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_LOGI(TAG,
             "UART bridge TX listo UART1 TX=%d RX=%d baud=%d",
             UART_BRIDGE_TX_GPIO,
             UART_BRIDGE_RX_GPIO,
             UART_BRIDGE_BAUDRATE);

    esp_err_t err = uart_driver_install(UART_BRIDGE_PORT,
                                        UART_BRIDGE_RX_BUF_SIZE,
                                        UART_BRIDGE_TX_BUF_SIZE,
                                        0,
                                        NULL,
                                        0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed err=0x%x", (unsigned)err);
        return err;
    }

    err = uart_param_config(UART_BRIDGE_PORT, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed err=0x%x", (unsigned)err);
        return err;
    }

    err = uart_set_pin(UART_BRIDGE_PORT,
                       UART_BRIDGE_TX_GPIO,
                       UART_BRIDGE_RX_GPIO,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed err=0x%x", (unsigned)err);
        return err;
    }

    return ESP_OK;
}

esp_err_t uart_bridge_send_json(const char *json)
{
    if (json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(json);

    if (len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (len >= UART_BRIDGE_MAX_JSON_LEN) {
        ESP_LOGE(TAG,
                 "JSON demasiado largo len=%u max=%u",
                 (unsigned)len,
                 (unsigned)UART_BRIDGE_MAX_JSON_LEN);
        return ESP_ERR_INVALID_SIZE;
    }

    int written = uart_write_bytes(UART_BRIDGE_PORT, json, len);

    if (written < 0) {
        ESP_LOGE(TAG, "uart_write_bytes failed");
        return ESP_FAIL;
    }

    if ((size_t)written != len) {
        ESP_LOGE(TAG,
                 "UART write incompleto written=%d len=%u",
                 written,
                 (unsigned)len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "JSON enviado por UART len=%u", (unsigned)len);

    return ESP_OK;
}