#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "cJSON.h"
#include "psa/crypto.h"
#include "config.h"
#include "mesh_payload.h"

static const char *TAG="ESPNOW_GATEWAY";
static const uint8_t s_router[6]=ROUTER_MAC_BYTES,s_pmk[16]=ESPNOW_PMK_BYTES,s_lmk[16]=ESPNOW_LMK_BYTES,s_key[16]=APP_AES_GCM_KEY_BYTES;
typedef struct {espnow_mesh_frame_t frame;int8_t rssi;} rx_item_t;
typedef struct {char node_id[4];uint32_t packet_id;uint16_t batch_id;bool valid;} duplicate_entry_t;
static QueueHandle_t s_queue;static duplicate_entry_t s_seen[24];static size_t s_seen_next;

/* La cola mantiene el callback Wi-Fi corto; descifrado y JSON ocurren en tarea. */

static bool duplicate(const sensor_packet_t *p)
{
    for(size_t i=0;i<24;i++)if(s_seen[i].valid&&strcmp(s_seen[i].node_id,p->node_id)==0&&s_seen[i].packet_id==p->packet_id&&s_seen[i].batch_id==p->batch_id)return true;
    duplicate_entry_t *e=&s_seen[s_seen_next++%24];memset(e,0,sizeof(*e));strlcpy(e->node_id,p->node_id,sizeof(e->node_id));e->packet_id=p->packet_id;e->batch_id=p->batch_id;e->valid=true;return false;
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
static esp_err_t decrypt(const secure_espnow_packet_t *s,sensor_packet_t *p)
{
    if(s->magic!=ESPNOW_APP_SEC_MAGIC||s->version!=ESPNOW_APP_SEC_VERSION||s->encrypted_len!=sizeof(*p))return ESP_ERR_INVALID_RESPONSE;
    secure_packet_aad_t aad={s->magic,s->version,s->batch_id,s->encrypted_len,{0}};memcpy(aad.iv,s->iv,12);
    psa_key_attributes_t a=PSA_KEY_ATTRIBUTES_INIT;psa_key_id_t k=0;psa_set_key_type(&a,PSA_KEY_TYPE_AES);psa_set_key_bits(&a,128);psa_set_key_usage_flags(&a,PSA_KEY_USAGE_DECRYPT);psa_set_key_algorithm(&a,PSA_ALG_GCM);
    if(psa_crypto_init()!=PSA_SUCCESS||psa_import_key(&a,s_key,16,&k)!=PSA_SUCCESS)return ESP_FAIL;
    uint8_t in[sizeof(*p)+16];memcpy(in,s->encrypted_payload,sizeof(*p));memcpy(in+sizeof(*p),s->auth_tag,16);size_t n=0;
    psa_status_t st=psa_aead_decrypt(k,PSA_ALG_GCM,s->iv,12,(uint8_t*)&aad,sizeof(aad),in,sizeof(in),(uint8_t*)p,sizeof(*p),&n);psa_destroy_key(k);psa_reset_key_attributes(&a);
    return(st==PSA_SUCCESS&&n==sizeof(*p)&&p->batch_id==s->batch_id)?ESP_OK:ESP_FAIL;
}
static void recv_cb(const esp_now_recv_info_t *info,const uint8_t *data,int len)
{
    if(!info||!data||len!=(int)sizeof(espnow_mesh_frame_t)||memcmp(info->src_addr,s_router,6)!=0)return;
    rx_item_t item={0};memcpy(&item.frame,data,sizeof(item.frame));item.rssi=info->rx_ctrl?info->rx_ctrl->rssi:MESH_RSSI_UNKNOWN;
    if(xQueueSend(s_queue,&item,0)!=pdTRUE)ESP_EARLY_LOGW(TAG,"Cola llena");
}
static void emit_json(const sensor_packet_t *p,const espnow_mesh_frame_t *f,int8_t rssi)
{
    /* Una linea JSON autocontenida por paquete para delimitarla en UART. */
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000ULL);uint32_t latency=now-p->node_tx_timestamp_ms;mpu_sample_t s=p->muestras[0];
    cJSON *root=cJSON_CreateObject(),*values=cJSON_CreateObject(),*route=cJSON_CreateObject();if(!root||!values||!route){cJSON_Delete(root);cJSON_Delete(values);cJSON_Delete(route);return;}
    cJSON_AddStringToObject(root,"node_id",p->node_id);cJSON_AddStringToObject(root,"src_mac",p->src_mac);cJSON_AddNumberToObject(root,"packet_id",p->packet_id);cJSON_AddNumberToObject(root,"batch_id",p->batch_id);
    int lqi = (rssi == MESH_RSSI_UNKNOWN) ? -1 : rssi_to_quality_255(rssi);
    cJSON_AddNumberToObject(root,"ts_gateway_ms",now);cJSON_AddNumberToObject(root,"node_tx_timestamp_ms",p->node_tx_timestamp_ms);cJSON_AddNumberToObject(root,"latency_ms",latency);cJSON_AddNumberToObject(root,"rssi",rssi);cJSON_AddNumberToObject(root,"lqi",lqi);cJSON_AddBoolToObject(root,"duplicate",false);cJSON_AddBoolToObject(root,"decrypt_ok",true);
    cJSON_AddNumberToObject(values,"synthetic_ax",s.ax/16384.0);cJSON_AddNumberToObject(values,"synthetic_ay",s.ay/16384.0);cJSON_AddNumberToObject(values,"synthetic_az",s.az/16384.0);cJSON_AddNumberToObject(values,"synthetic_gx",s.gx/131.0);cJSON_AddNumberToObject(values,"synthetic_gy",s.gy/131.0);cJSON_AddNumberToObject(values,"synthetic_gz",s.gz/131.0);cJSON_AddItemToObject(root,"values",values);
    cJSON_AddStringToObject(route,"relay_node_id",f->relay_node_id);cJSON_AddNumberToObject(route,"hop_count",f->hop_count);cJSON_AddNumberToObject(route,"first_hop_rssi",f->first_hop_rssi);cJSON_AddNumberToObject(route,"last_hop_rssi",rssi);cJSON_AddItemToObject(root,"route",route);
    char *line=cJSON_PrintUnformatted(root);if(line){uart_write_bytes(UART_PORT_NUM,line,strlen(line));uart_write_bytes(UART_PORT_NUM,"\n",1);ESP_LOGI(TAG,"JSON generado: %s",line);cJSON_free(line);}cJSON_Delete(root);
}
static void process_task(void *arg)
{
    (void)arg;rx_item_t item;while(1){xQueueReceive(s_queue,&item,portMAX_DELAY);espnow_mesh_frame_t *f=&item.frame;
        if(f->mesh_magic!=ESPNOW_MESH_MAGIC||f->mesh_version!=ESPNOW_MESH_VERSION||f->mesh_type!=ESPNOW_MESH_TYPE_DATA||f->secure_len!=sizeof(secure_espnow_packet_t))continue;
        secure_espnow_packet_t sec;memcpy(&sec,f->secure_payload,sizeof(sec));sensor_packet_t p={0};if(decrypt(&sec,&p)!=ESP_OK){ESP_LOGW(TAG,"AES-GCM fallo; medicion descartada");continue;}
        if(strncmp(p.node_id,f->origin_node_id,sizeof(p.node_id))!=0||p.packet_id!=f->packet_id_shadow||p.batch_id!=f->batch_id_shadow){ESP_LOGW(TAG,"Metadata de malla inconsistente");continue;}
        if(duplicate(&p)){ESP_LOGW(TAG,"Duplicado descartado node=%s packet=%"PRIu32" batch=%u",p.node_id,p.packet_id,p.batch_id);continue;}
        ESP_LOGI(TAG,"AES-GCM OK origin=%s packet_id=%"PRIu32" batch_id=%u",p.node_id,p.packet_id,p.batch_id);emit_json(&p,f,item.rssi);
    }
}
static void print_wifi_channel(void)
{
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;

    esp_err_t err = esp_wifi_get_channel(&primary, &second);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Canal WiFi/ESP-NOW actual: %u", primary);
    } else {
        ESP_LOGE(TAG, "No se pudo leer el canal WiFi: %s", esp_err_to_name(err));
    }
}

static esp_err_t init_radio(void)
{
    esp_err_t e = nvs_flash_init();

    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase");
        e = nvs_flash_init();
    }

    ESP_RETURN_ON_ERROR(e, TAG, "nvs");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "events");

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t c = WIFI_INIT_CONFIG_DEFAULT();

    ESP_RETURN_ON_ERROR(esp_wifi_init(&c), TAG, "wifi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE), TAG, "channel");

    print_wifi_channel();
    ESP_LOGI(TAG, "ESPNOW Gateway listo. Esperando paquetes del router N3...");
    ESP_LOGI(TAG, "Canal configurado por config.h: %d", ESPNOW_CHANNEL);

    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "esp-now");
    ESP_RETURN_ON_ERROR(esp_now_set_pmk(s_pmk), TAG, "pmk");

    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, s_router, 6);
    memcpy(p.lmk, s_lmk, 16);

    p.channel = ESPNOW_CHANNEL;
    p.ifidx = WIFI_IF_STA;
    p.encrypt = true;

    ESP_RETURN_ON_ERROR(esp_now_add_peer(&p), TAG, "router peer");

    return esp_now_register_recv_cb(recv_cb);
}
void app_main(void)
{
    uart_config_t u={.baud_rate=UART_BAUDRATE,.data_bits=UART_DATA_8_BITS,.parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,.flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM,1024,0,0,NULL,0));ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM,&u));ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM,UART_TX_GPIO,UART_RX_GPIO,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE));
    s_queue=xQueueCreate(20,sizeof(rx_item_t));ESP_ERROR_CHECK(s_queue?ESP_OK:ESP_ERR_NO_MEM);ESP_ERROR_CHECK(init_radio());xTaskCreate(process_task,"gateway_process",7168,NULL,5,NULL);
}
