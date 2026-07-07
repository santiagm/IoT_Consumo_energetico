#include "mqtt_handler.h"
#include <stdio.h>
#include <string.h>
#include "mqtt_client.h"
#include "esp_log.h"
#include "config.h"

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: s_connected = true; ESP_LOGI(TAG, "MQTT conectado"); break;
    case MQTT_EVENT_DISCONNECTED: s_connected = false; ESP_LOGW(TAG, "MQTT desconectado"); break;
    case MQTT_EVENT_ERROR: ESP_LOGE(TAG, "MQTT error"); break;
    default: break;
    }
}

esp_err_t mqtt_app_start(void)
{
    char uri[96];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", THINGSBOARD_HOST, THINGSBOARD_PORT);
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.username = THINGSBOARD_TOKEN,
        .credentials.client_id = MQTT_CLIENT_ID,
    };
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) return ESP_FAIL;
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    return esp_mqtt_client_start(s_client);
}

bool mqtt_is_connected(void) { return s_connected; }

esp_err_t mqtt_publish_telemetry(const char *json)
{
    if (!s_client || !s_connected || !json) return ESP_ERR_INVALID_STATE;
    int msg_id = esp_mqtt_client_publish(s_client, THINGSBOARD_TOPIC, json, 0, 1, 0);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}
