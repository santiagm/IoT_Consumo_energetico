#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "config.h"
#include "espnow_handler.h"
static const char *TAG="ROUTER_N3";
static const uint8_t s_n1[6]=N1_MAC_BYTES, s_n2[6]=N2_MAC_BYTES, s_gateway[6]=GATEWAY_MAC_BYTES;
static const uint8_t s_pmk[16]=ESPNOW_PMK_BYTES, s_lmk[16]=ESPNOW_LMK_BYTES;
static QueueHandle_t s_queue;

static bool known_node(const uint8_t *mac) { return memcmp(mac,s_n1,6)==0 || memcmp(mac,s_n2,6)==0; }
static void recv_cb(const esp_now_recv_info_t *info,const uint8_t *data,int len)
{
    if(!info||!data||len!=(int)sizeof(espnow_mesh_frame_t)||!s_queue||!known_node(info->src_addr)) return;
    router_rx_item_t item={0}; memcpy(&item.frame,data,sizeof(item.frame)); memcpy(item.sender_mac,info->src_addr,6);
    item.rssi=(info->rx_ctrl != NULL) ? info->rx_ctrl->rssi : MESH_RSSI_UNKNOWN;
    if(xQueueSend(s_queue,&item,0)!=pdTRUE) ESP_EARLY_LOGW(TAG,"Cola de relay llena");
}
static void send_cb(const esp_now_send_info_t *info,esp_now_send_status_t status)
{ (void)info; ESP_LOGI(TAG,"TX relay hacia Gateway -> %s",status==ESP_NOW_SEND_SUCCESS?"OK":"FALLO"); }
static esp_err_t add_peer(const uint8_t mac[6])
{
    esp_now_peer_info_t p={0}; memcpy(p.peer_addr,mac,6); memcpy(p.lmk,s_lmk,16);
    p.channel=ESPNOW_CHANNEL; p.ifidx=WIFI_IF_STA; p.encrypt=true;
    return esp_now_add_peer(&p);
}
esp_err_t espnow_handler_init(QueueHandle_t queue)
{
    s_queue=queue; esp_err_t e=nvs_flash_init();
    if(e==ESP_ERR_NVS_NO_FREE_PAGES||e==ESP_ERR_NVS_NEW_VERSION_FOUND){ESP_RETURN_ON_ERROR(nvs_flash_erase(),TAG,"nvs erase");e=nvs_flash_init();}
    ESP_RETURN_ON_ERROR(e,TAG,"nvs");
    e=esp_netif_init(); if(e!=ESP_OK&&e!=ESP_ERR_INVALID_STATE)return e;
    e=esp_event_loop_create_default(); if(e!=ESP_OK&&e!=ESP_ERR_INVALID_STATE)return e;
    esp_netif_create_default_wifi_sta(); wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg),TAG,"wifi"); ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA),TAG,"mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(),TAG,"start"); ESP_RETURN_ON_ERROR(esp_wifi_set_channel(ESPNOW_CHANNEL,WIFI_SECOND_CHAN_NONE),TAG,"channel");
    ESP_RETURN_ON_ERROR(esp_now_init(),TAG,"esp-now"); ESP_RETURN_ON_ERROR(esp_now_set_pmk(s_pmk),TAG,"pmk");
    ESP_RETURN_ON_ERROR(add_peer(s_n1),TAG,"N1 peer"); ESP_RETURN_ON_ERROR(add_peer(s_n2),TAG,"N2 peer"); ESP_RETURN_ON_ERROR(add_peer(s_gateway),TAG,"gateway peer");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(recv_cb),TAG,"recv cb"); ESP_RETURN_ON_ERROR(esp_now_register_send_cb(send_cb),TAG,"send cb");
    return ESP_OK;
}
esp_err_t espnow_handler_send_gateway(const espnow_mesh_frame_t *frame)
{ return esp_now_send(s_gateway,(const uint8_t*)frame,sizeof(*frame)); }
