#include "wifi_handler.h"

#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/ip_addr.h"
#include "config.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;

static void wifi_set_manual_dns(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == NULL) {
        ESP_LOGW(TAG, "No se encontro netif WIFI_STA_DEF para DNS manual");
        return;
    }

    esp_netif_dns_info_t dns_main = {0};
    dns_main.ip.type = ESP_IPADDR_TYPE_V4;
    dns_main.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);

    esp_netif_dns_info_t dns_backup = {0};
    dns_backup.ip.type = ESP_IPADDR_TYPE_V4;
    dns_backup.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);

    esp_err_t r1 = esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns_main);
    esp_err_t r2 = esp_netif_set_dns_info(sta, ESP_NETIF_DNS_BACKUP, &dns_backup);
    ESP_LOGI(TAG, "DNS manual configurado ret_main=%s ret_backup=%s", esp_err_to_name(r1), esp_err_to_name(r2));
}

static void wifi_test_dns_thingsboard(void)
{
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int err = getaddrinfo(THINGSBOARD_HOST, NULL, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGW(TAG, "DNS TEST fallo: %s getaddrinfo=%d", THINGSBOARD_HOST, err);
        return;
    }

    char ip_str[INET_ADDRSTRLEN] = {0};
    struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, "DNS TEST OK: %s -> %s", THINGSBOARD_HOST, ip_str);

    freeaddrinfo(res);
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_retry_num < 20) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "WiFi desconectado. Reintentando %d/20", s_retry_num);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_num = 0;
        wifi_set_manual_dns();
        wifi_test_dns_thingsboard();
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_connect_blocking(void)
{
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(30000)
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi conectado");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "WiFi no conectado");
    return ESP_FAIL;
}
