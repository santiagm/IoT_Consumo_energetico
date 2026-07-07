#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "config.h"
#include "espnow_handler.h"

static const char *TAG = "ESPNOW_NODE_ESC2";

static const uint8_t s_gateway_mac[ESP_NOW_ETH_ALEN] = GATEWAY_MAC_BYTES;
static bool s_espnow_ready = false;

static void espnow_send_cb(const esp_now_send_info_t *tx_info,
                           esp_now_send_status_t status)
{
    if (tx_info == NULL) {
        ESP_LOGW(TAG, "Callback ESP-NOW sin informacion de envio");
        return;
    }

    const uint8_t *mac = tx_info->des_addr;
    ESP_LOGI(TAG,
             "TX ESP-NOW a %02X:%02X:%02X:%02X:%02X:%02X -> %s",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             status == ESP_NOW_SEND_SUCCESS ? "OK" : "FALLO");
}

static esp_err_t init_nvs_storage(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }

    return err;
}

static esp_err_t wifi_init_sta_radio_only(void)
{
    esp_err_t err = init_nvs_storage();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGW(TAG, "No se pudo crear default WIFI STA o ya existe");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        return err;
    }

    uint8_t sta_mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, sta_mac) == ESP_OK) {
        ESP_LOGI(TAG, "MAC STA del NODO: %02X:%02X:%02X:%02X:%02X:%02X",
                 sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);
    }

    err = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo fijar canal ESP-NOW: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static esp_err_t init_espnow_plain_peer(void)
{
    esp_err_t err = esp_now_init();
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        return err;
    }

    err = esp_now_register_send_cb(espnow_send_cb);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_INTERNAL) {
        ESP_LOGW(TAG, "No se pudo registrar callback ESP-NOW: %s", esp_err_to_name(err));
    }

    esp_now_peer_info_t peer_info = {0};
    memcpy(peer_info.peer_addr, s_gateway_mac, ESP_NOW_ETH_ALEN);
    peer_info.channel = ESPNOW_CHANNEL;
    peer_info.ifidx = WIFI_IF_STA;
    peer_info.encrypt = false;

    if (esp_now_is_peer_exist(s_gateway_mac)) {
        err = esp_now_mod_peer(&peer_info);
    } else {
        err = esp_now_add_peer(&peer_info);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo agregar/modificar peer ESP-NOW: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "Peer gateway ESP-NOW listo: %02X:%02X:%02X:%02X:%02X:%02X canal=%d",
             s_gateway_mac[0], s_gateway_mac[1], s_gateway_mac[2],
             s_gateway_mac[3], s_gateway_mac[4], s_gateway_mac[5],
             ESPNOW_CHANNEL);

    return ESP_OK;
}

esp_err_t espnow_handler_init(void)
{
    if (s_espnow_ready) {
        return ESP_OK;
    }

    esp_err_t err = wifi_init_sta_radio_only();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi STA init fallo: %s", esp_err_to_name(err));
        return err;
    }

    err = init_espnow_plain_peer();
    if (err != ESP_OK) {
        return err;
    }

    s_espnow_ready = true;
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

esp_err_t espnow_handler_send(const uint8_t *data, size_t len)
{
    if (!s_espnow_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Enviando paquete ESP-NOW plano len=%u", (unsigned)len);

    esp_err_t err = esp_now_send(s_gateway_mac, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_send fallo inmediatamente: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(5));
    return ESP_OK;
}
