#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "config.h"
#include "espnow_handler.h"

static const char *TAG = "NODO_ESPNOW";
static const uint8_t s_router_mac[6] = ROUTER_MAC_BYTES;
static const uint8_t s_pmk[16] = ESPNOW_PMK_BYTES;
static const uint8_t s_lmk[16] = ESPNOW_LMK_BYTES;
static SemaphoreHandle_t s_tx_done;
static bool s_tx_ok;

static void send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    s_tx_ok = (status == ESP_NOW_SEND_SUCCESS);
    if (s_tx_done) xSemaphoreGive(s_tx_done);
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t espnow_handler_init(void)
{
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "nvs init");
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE), TAG, "channel");
    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "esp-now init");
    ESP_RETURN_ON_ERROR(esp_now_set_pmk(s_pmk), TAG, "pmk");
    ESP_RETURN_ON_ERROR(esp_now_register_send_cb(send_cb), TAG, "send cb");

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_router_mac, sizeof(peer.peer_addr));
    memcpy(peer.lmk, s_lmk, sizeof(peer.lmk));
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = true;
    ESP_RETURN_ON_ERROR(esp_now_add_peer(&peer), TAG, "router peer");
    s_tx_done = xSemaphoreCreateBinary();
    return s_tx_done ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t espnow_handler_send(const uint8_t *data, size_t len)
{
    s_tx_ok = false;
    ESP_RETURN_ON_ERROR(esp_now_send(s_router_mac, data, len), TAG, "send");
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    ESP_LOGI(TAG, "TX ESP-NOW cifrada a N3 -> %s", s_tx_ok ? "OK" : "FALLO");
    return s_tx_ok ? ESP_OK : ESP_FAIL;
}
