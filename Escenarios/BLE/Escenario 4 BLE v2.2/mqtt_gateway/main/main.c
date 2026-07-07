#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
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

#include "cJSON.h"

#include "config.h"
#include "telemetry_packet.h"

static const char *TAG = "MQTT_GATEWAY";

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static EventGroupHandle_t s_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0
#define MQTT_CONNECTED_BIT BIT1

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
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

static char *build_thingsboard_json(const sensor_packet_t *packet)
{
    if (packet == NULL) {
        return NULL;
    }

    /*
     * ThingsBoard recibe un arreglo JSON con una sola muestra lógica.
     * Se mantiene sensor_packet_t completo para uniformidad experimental, pero
     * el JSON publica un único objeto usando la primera muestra del batch.
     */
    const mpu_sample_t *m = &packet->muestras[0];

    const double accel_x_g = (double)m->ax / 16384.0;
    const double accel_y_g = (double)m->ay / 16384.0;
    const double accel_z_g = (double)m->az / 16384.0;

    const double gyro_x_dps = (double)m->gx / 131.0;
    const double gyro_y_dps = (double)m->gy / 131.0;
    const double gyro_z_dps = (double)m->gz / 131.0;

    cJSON *root = cJSON_CreateArray();
    if (root == NULL) {
        return NULL;
    }

    cJSON *entry = cJSON_CreateObject();
    cJSON *values = cJSON_CreateObject();
    if (entry == NULL || values == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(entry);
        cJSON_Delete(values);
        return NULL;
    }

    cJSON_AddNumberToObject(entry, "ts", (double)(esp_timer_get_time() / 1000ULL));

    cJSON_AddStringToObject(values, "src_mac", packet->src_mac);
    cJSON_AddNumberToObject(values, "node_start_timestamp_ms", (double)packet->start_timestamp_ms);
    cJSON_AddNumberToObject(values, "batch_id", packet->batch_id);

    cJSON_AddNumberToObject(values, "accel_x_g", accel_x_g);
    cJSON_AddNumberToObject(values, "accel_y_g", accel_y_g);
    cJSON_AddNumberToObject(values, "accel_z_g", accel_z_g);

    cJSON_AddNumberToObject(values, "gyro_x_dps", gyro_x_dps);
    cJSON_AddNumberToObject(values, "gyro_y_dps", gyro_y_dps);
    cJSON_AddNumberToObject(values, "gyro_z_dps", gyro_z_dps);

    cJSON_AddItemToObject(entry, "values", values);
    cJSON_AddItemToArray(root, entry);

    char *json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_string;
}


static bool read_exact_uart(uint8_t *dst, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        int r = uart_read_bytes(UART_BRIDGE_PORT,
                                dst + offset,
                                len - offset,
                                pdMS_TO_TICKS(5000));
        if (r < 0) {
            return false;
        }
        if (r == 0) {
            offset = 0;
            continue;
        }
        offset += (size_t)r;
    }
    return true;
}

static void uart_mqtt_task(void *pvParameter)
{
    (void)pvParameter;
    sensor_packet_t packet;

    while (1) {
        memset(&packet, 0, sizeof(packet));
        if (!read_exact_uart((uint8_t *)&packet, sizeof(packet))) {
            continue;
        }

        EventBits_t bits = xEventGroupGetBits(s_event_group);
        if (s_mqtt_client == NULL || ((bits & MQTT_CONNECTED_BIT) == 0)) {
            ESP_LOGW(TAG, "MQTT no conectado; paquete UART descartado batch=%u", packet.batch_id);
            continue;
        }

        char *json_string = build_thingsboard_json(&packet);
        if (json_string == NULL) {
            ESP_LOGW(TAG, "No se pudo crear JSON batch=%u", packet.batch_id);
            continue;
        }

        int msg_id = esp_mqtt_client_publish(s_mqtt_client,
                                             THINGSBOARD_TOPIC,
                                             json_string,
                                             0,
                                             1,
                                             0);

#if GATEWAY_DEBUG_LOGS
        ESP_LOGI(TAG, "UART -> ThingsBoard batch=%u msg_id=%d", packet.batch_id, msg_id);
#endif

        cJSON_free(json_string);
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

    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        ESP_LOGE(TAG, "No se pudo crear EventGroup");
        return;
    }

    ESP_ERROR_CHECK(uart_bridge_init());
    ESP_ERROR_CHECK(wifi_init_sta());
    ESP_ERROR_CHECK(mqtt_start());

    xTaskCreate(uart_mqtt_task, "uart_mqtt", 8192, NULL, 5, NULL);
}
