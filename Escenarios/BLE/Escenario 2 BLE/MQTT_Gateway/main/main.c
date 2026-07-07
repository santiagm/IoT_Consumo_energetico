#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "config.h"
#include "wifi_handler.h"
#include "mqtt_handler.h"
#include "uart_bridge_receiver.h"

static const char *TAG = "GW_MQTT_UART";

static QueueHandle_t s_json_queue = NULL;
static uint32_t s_uart_rx = 0;
static uint32_t s_published = 0;
static uint32_t s_mqtt_errors = 0;
static uint32_t s_requeued = 0;

static esp_err_t gateway_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static void log_metrics(void)
{
    ESP_LOGI(TAG,
             "Metricas UART_RX=%" PRIu32 " PUB=%" PRIu32 " MQTT_ERR=%" PRIu32 " REQUEUE=%" PRIu32 " MQTT=%s",
             s_uart_rx,
             s_published,
             s_mqtt_errors,
             s_requeued,
             mqtt_is_connected() ? "OK" : "OFF");
}

static void mqtt_publish_task(void *arg)
{
    (void)arg;
    uart_json_msg_t msg;

    while (true) {
        if (xQueueReceive(s_json_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        s_uart_rx++;
        ESP_LOGI(TAG, "JSON listo para ThingsBoard: %s", msg.json);

        while (!mqtt_is_connected()) {
            ESP_LOGW(TAG, "MQTT no conectado. Esperando para publicar JSON pendiente...");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }

        esp_err_t ret = mqtt_publish_telemetry(msg.json);
        if (ret == ESP_OK) {
            s_published++;
            ESP_LOGI(TAG, "Publicado ThingsBoard desde UART");
        } else {
            s_mqtt_errors++;
            ESP_LOGE(TAG, "Error publicando. Se reintentara luego");

            if (xQueueSendToFront(s_json_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
                s_requeued++;
            } else {
                ESP_LOGE(TAG, "No se pudo devolver JSON a la cola");
            }

            vTaskDelay(pdMS_TO_TICKS(3000));
        }

        log_metrics();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando MQTT Gateway UART");

    ESP_ERROR_CHECK(gateway_nvs_init());

    s_json_queue = xQueueCreate(UART_QUEUE_LENGTH, sizeof(uart_json_msg_t));
    if (s_json_queue == NULL) {
        ESP_LOGE(TAG, "No se pudo crear cola JSON");
        esp_restart();
    }

    ESP_ERROR_CHECK(uart_bridge_receiver_init(s_json_queue));

    ESP_LOGI(TAG, "Conectando WiFi...");
    ESP_ERROR_CHECK(wifi_connect_blocking());

    ESP_LOGI(TAG, "Iniciando MQTT ThingsBoard...");
    ESP_ERROR_CHECK(mqtt_app_start());

    for (int i = 0; i < 40 && !mqtt_is_connected(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    if (mqtt_is_connected()) {
        ESP_LOGI(TAG, "MQTT listo");
    } else {
        ESP_LOGW(TAG, "MQTT no conectado todavia; se reintentara por cliente MQTT");
    }

    xTaskCreate(mqtt_publish_task, "mqtt_publish_task", 6144, NULL, 5, NULL);

    ESP_LOGI(TAG, "MQTT Gateway listo. UART RX=%d TX=%d baud=%d",
             UART_BRIDGE_RX_GPIO, UART_BRIDGE_TX_GPIO, UART_BRIDGE_BAUD);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        log_metrics();
    }
}
