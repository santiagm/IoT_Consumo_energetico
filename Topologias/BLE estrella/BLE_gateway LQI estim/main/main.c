#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "driver/uart.h"

#include "ble_scanner.h"
#include "config.h"
#include "telemetry_packet.h"

static const char *TAG = "GW_BLE_UART";

QueueHandle_t s_packet_queue = NULL;

#if GATEWAY_ENABLE_MQTT
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static EventGroupHandle_t s_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0
#define MQTT_CONNECTED_BIT BIT1

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi desconectado, reintentando...");
        xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | MQTT_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi conectado");
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT conectado a ThingsBoard");
        xEventGroupSetBits(s_event_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT desconectado");
        xEventGroupClearBits(s_event_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT_EVENT_ERROR");
        if (event != NULL && event->error_handle != NULL) {
            ESP_LOGW(TAG, "MQTT error_type=%d", event->error_handle->error_type);
        }
        break;

    default:
        break;
    }
}

static esp_err_t wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

#if GATEWAY_WIFI_POWER_SAVE
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
#else
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#endif

    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           pdMS_TO_TICKS(20000));

    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "Timeout esperando conexion WiFi");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static esp_err_t mqtt_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.hostname = THINGSBOARD_HOST,
        .broker.address.port = THINGSBOARD_PORT,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.username = THINGSBOARD_TOKEN,
        .credentials.client_id = MQTT_CLIENT_ID,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "No se pudo crear cliente MQTT");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt_client,
                                                   ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler,
                                                   NULL));

    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           MQTT_CONNECTED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           pdMS_TO_TICKS(15000));

    if ((bits & MQTT_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "MQTT aun no conectado; el cliente seguira reintentando");
    }

    return ESP_OK;
}
#endif

static uint32_t gateway_time_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static int rssi_to_quality_255(int8_t rssi)
{
    if (rssi <= -100) {
        return 0;
    }

    if (rssi >= -50) {
        return 255;
    }

    return ((int)rssi + 100) * 255 / 50;
}

static esp_err_t build_uart_json(const gateway_packet_t *rx, char *out, size_t out_len)
{
    if (rx == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const sensor_packet_t *packet = &rx->packet;
    const mpu_sample_t *m = &packet->muestras[0];

    const uint32_t ts_gateway_ms = gateway_time_ms();
    const uint32_t node_tx_ms = packet->start_timestamp_ms;
    const uint32_t latency_ms = (ts_gateway_ms >= node_tx_ms) ? (ts_gateway_ms - node_tx_ms) : 0;
    const int lqi = (rx->rssi == 127) ? -1 : rssi_to_quality_255(rx->rssi);

    const double synthetic_ax = (double)m->ax / 16384.0;
    const double synthetic_ay = (double)m->ay / 16384.0;
    const double synthetic_az = (double)m->az / 16384.0;
    const double synthetic_gx = (double)m->gx / 131.0;
    const double synthetic_gy = (double)m->gy / 131.0;
    const double synthetic_gz = (double)m->gz / 131.0;

    const char *node_id = (packet->node_id[0] != '\0') ? packet->node_id : "N?";

    int written = snprintf(out,
                           out_len,
                           "{\"node_id\":\"%s\",\"src_mac\":\"%s\","
                           "\"packet_id\":%u,\"batch_id\":%u,"
                           "\"ts_gateway_ms\":%" PRIu32 ","
                           "\"node_tx_timestamp_ms\":%" PRIu32 ","
                           "\"latency_ms\":%" PRIu32 ","
                           "\"rssi\":%d,\"lqi\":%d,"
                           "\"duplicate\":%s,\"decrypt_ok\":%s,"
                           "\"values\":{" 
                           "\"synthetic_ax\":%.5f,\"synthetic_ay\":%.5f,\"synthetic_az\":%.5f,"
                           "\"synthetic_gx\":%.5f,\"synthetic_gy\":%.5f,\"synthetic_gz\":%.5f}}",
                           node_id,
                           packet->src_mac,
                           packet->batch_id,
                           packet->batch_id,
                           ts_gateway_ms,
                           node_tx_ms,
                           latency_ms,
                           (int)rx->rssi,
                           lqi,
                           rx->duplicate ? "true" : "false",
                           rx->decrypt_ok ? "true" : "false",
                           synthetic_ax,
                           synthetic_ay,
                           synthetic_az,
                           synthetic_gx,
                           synthetic_gy,
                           synthetic_gz);

    if (written < 0 || (size_t)written >= out_len) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t uart_bridge_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = UART_BRIDGE_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_BRIDGE_PORT,
                                        UART_BRIDGE_RX_BUF_SIZE,
                                        UART_BRIDGE_TX_BUF_SIZE,
                                        0,
                                        NULL,
                                        0));
    ESP_ERROR_CHECK(uart_param_config(UART_BRIDGE_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_BRIDGE_PORT,
                                 UART_BRIDGE_TX_GPIO,
                                 UART_BRIDGE_RX_GPIO,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART bridge listo: TX=GPIO%d RX=GPIO%d baud=%d",
             UART_BRIDGE_TX_GPIO, UART_BRIDGE_RX_GPIO, UART_BRIDGE_BAUD_RATE);
    return ESP_OK;
}

static void packet_consumer_task(void *pvParameter)
{
    (void)pvParameter;
    gateway_packet_t rx;
    char json_line[512];

    while (1) {
        if (xQueueReceive(s_packet_queue, &rx, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        esp_err_t json_err = build_uart_json(&rx, json_line, sizeof(json_line));
        if (json_err != ESP_OK) {
            ESP_LOGW(TAG, "No se pudo crear JSON node=%s batch=%u err=%s",
                     rx.packet.node_id, rx.packet.batch_id, esp_err_to_name(json_err));
            continue;
        }

#if GATEWAY_ENABLE_MQTT
        EventBits_t bits = xEventGroupGetBits(s_event_group);
        if (s_mqtt_client == NULL || ((bits & MQTT_CONNECTED_BIT) == 0)) {
            ESP_LOGW(TAG, "MQTT no conectado; paquete BLE descartado batch=%u", rx.packet.batch_id);
            continue;
        }

        int msg_id = esp_mqtt_client_publish(s_mqtt_client,
                                             THINGSBOARD_TOPIC,
                                             json_line,
                                             0,
                                             1,
                                             0);

#if GATEWAY_DEBUG_LOGS
        ESP_LOGI(TAG, "Publicado ThingsBoard node=%s batch=%u msg_id=%d",
                 rx.packet.node_id, rx.packet.batch_id, msg_id);
#endif
#else
        int written = uart_write_bytes(UART_BRIDGE_PORT, json_line, strlen(json_line));
        written += uart_write_bytes(UART_BRIDGE_PORT, "\n", 1);
        if (written <= 1) {
            ESP_LOGW(TAG, "UART no envio JSON node=%s batch=%u", rx.packet.node_id, rx.packet.batch_id);
        }
#if GATEWAY_DEBUG_LOGS
        ESP_LOGI(TAG, "JSON generado: %s", json_line);
        ESP_LOGI(TAG, "BLE estrella -> UART node=%s batch=%u rssi=%d bytes=%d",
                 rx.packet.node_id, rx.packet.batch_id, (int)rx.rssi, written);
#endif
#endif
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);

    s_packet_queue = xQueueCreate(GATEWAY_PACKET_QUEUE_LEN, sizeof(gateway_packet_t));
    if (s_packet_queue == NULL) {
        ESP_LOGE(TAG, "No se pudo crear la cola de paquetes");
        return;
    }

    ESP_ERROR_CHECK(uart_bridge_init());

#if GATEWAY_ENABLE_MQTT
    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        ESP_LOGE(TAG, "No se pudo crear EventGroup");
        return;
    }

    ESP_ERROR_CHECK(wifi_init_sta());
    ESP_ERROR_CHECK(mqtt_start());
#else
    ESP_LOGI(TAG, "BLE_gateway en modo puente BLE->UART. WiFi/MQTT desactivado.");
#endif

    xTaskCreate(packet_consumer_task, "packet_consumer", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "Iniciando topologia estrella BLE segura: N1/N2/N3 -> BLE_gateway -> UART GPIO4...");
    ESP_ERROR_CHECK(ble_scanner_init());
}
