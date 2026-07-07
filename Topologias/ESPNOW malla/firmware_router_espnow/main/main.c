#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "psa/crypto.h"
#include "config.h"
#include "espnow_handler.h"
static const char *TAG="ROUTER_N3";
static const uint8_t s_key[16]=APP_AES_GCM_KEY_BYTES;
static QueueHandle_t s_forward_queue;
static uint32_t s_packet_id; static uint16_t s_batch_id;

/* El router nunca descifra N1/N2: conserva AES-GCM extremo a extremo. */

static esp_err_t encrypt_packet(const sensor_packet_t *p,secure_espnow_packet_t *s)
{
    memset(s,0,sizeof(*s)); s->magic=ESPNOW_APP_SEC_MAGIC;s->version=ESPNOW_APP_SEC_VERSION;s->batch_id=p->batch_id;s->encrypted_len=sizeof(*p);
    uint8_t mac[6];ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA,mac),TAG,"MAC");s->iv[0]='E';s->iv[1]='N';memcpy(&s->iv[2],&mac[2],4);memcpy(&s->iv[6],&p->batch_id,2);memcpy(&s->iv[8],&p->node_tx_timestamp_ms,4);
    secure_packet_aad_t aad={s->magic,s->version,s->batch_id,s->encrypted_len,{0}};memcpy(aad.iv,s->iv,AES_GCM_IV_LEN);
    psa_key_attributes_t a=PSA_KEY_ATTRIBUTES_INIT;psa_key_id_t k=0;psa_set_key_type(&a,PSA_KEY_TYPE_AES);psa_set_key_bits(&a,128);psa_set_key_usage_flags(&a,PSA_KEY_USAGE_ENCRYPT);psa_set_key_algorithm(&a,PSA_ALG_GCM);
    if(psa_crypto_init()!=PSA_SUCCESS||psa_import_key(&a,s_key,16,&k)!=PSA_SUCCESS)return ESP_FAIL;
    uint8_t out[sizeof(*p)+AES_GCM_TAG_LEN];size_t n=0;psa_status_t st=psa_aead_encrypt(k,PSA_ALG_GCM,s->iv,12,(uint8_t*)&aad,sizeof(aad),(uint8_t*)p,sizeof(*p),out,sizeof(out),&n);psa_destroy_key(k);psa_reset_key_attributes(&a);
    if (st != PSA_SUCCESS || n != sizeof(out)) {
    return ESP_FAIL;
}

    memcpy(s->encrypted_payload, out, sizeof(*p));
    memcpy(s->auth_tag, out + sizeof(*p), AES_GCM_TAG_LEN);

    return ESP_OK;
}
static void forward_task(void *arg)
{
    /* El callback solo encola; esta tarea valida y actualiza la ruta. */
    (void)arg;router_rx_item_t item;uint8_t own[6];esp_wifi_get_mac(WIFI_IF_STA,own);
    while(1){if(xQueueReceive(s_forward_queue,&item,portMAX_DELAY)!=pdTRUE)continue;
        if(item.frame.mesh_magic!=ESPNOW_MESH_MAGIC||item.frame.mesh_version!=ESPNOW_MESH_VERSION||item.frame.mesh_type!=ESPNOW_MESH_TYPE_DATA||item.frame.secure_len!=sizeof(secure_espnow_packet_t))continue;
        strlcpy(item.frame.relay_node_id,NODE_ID,sizeof(item.frame.relay_node_id));memcpy(item.frame.relay_mac,own,6);item.frame.hop_count=1;item.frame.first_hop_rssi=item.rssi;
        ESP_LOGI(TAG,
         "Reenviando paquete origin=%s packet_id=%" PRIu32 " batch_id=%" PRIu16 " hop=%u rssi=%d hacia Gateway",
         item.frame.origin_node_id,
         item.frame.packet_id_shadow,
         item.frame.batch_id_shadow,
         item.frame.hop_count,
         item.rssi);
        espnow_handler_send_gateway(&item.frame);
    }
}
static void own_sensor_task(void *arg)
{
    /* N3 tambien actua como origen y usa el mismo formato cifrado. */
    (void)arg;uint8_t mac[6];esp_wifi_get_mac(WIFI_IF_STA,mac);
    while(1){sensor_packet_t p={0};strlcpy(p.node_id,NODE_ID,sizeof(p.node_id));snprintf(p.src_mac,sizeof(p.src_mac),"%02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);p.packet_id=++s_packet_id;p.batch_id=++s_batch_id;p.node_tx_timestamp_ms=(uint32_t)(esp_timer_get_time()/1000ULL);
        uint32_t r=esp_random();p.muestras[0]=(mpu_sample_t){120+(r&63),-80+((r>>6)&63),16384+((r>>12)&127),5+((r>>19)&31),-3+((r>>24)&31),2+((r>>29)&7)};
        secure_espnow_packet_t sec;espnow_mesh_frame_t f={0};if(encrypt_packet(&p,&sec)==ESP_OK){f.mesh_magic=ESPNOW_MESH_MAGIC;f.mesh_version=ESPNOW_MESH_VERSION;f.mesh_type=ESPNOW_MESH_TYPE_DATA;strlcpy(f.origin_node_id,NODE_ID,4);memcpy(f.origin_mac,mac,6);strlcpy(f.relay_node_id,NODE_ID,4);memcpy(f.relay_mac,mac,6);f.hop_count=0;f.first_hop_rssi=MESH_RSSI_UNKNOWN;f.last_hop_rssi=MESH_RSSI_UNKNOWN;f.packet_id_shadow=p.packet_id;f.batch_id_shadow=p.batch_id;f.secure_len=sizeof(sec);memcpy(f.secure_payload,&sec,sizeof(sec));
            ESP_LOGI(TAG,
            "Enviando paquete propio node_id=%s packet_id=%" PRIu32 " batch_id=%" PRIu16 " hacia Gateway",
            p.node_id,
            p.packet_id,
            p.batch_id);
            espnow_handler_send_gateway(&f);}vTaskDelay(pdMS_TO_TICKS(ROUTER_SEND_PERIOD_MS));}
}
void app_main(void)
{
    s_forward_queue=xQueueCreate(16,sizeof(router_rx_item_t));
    ESP_ERROR_CHECK(s_forward_queue?ESP_OK:ESP_ERR_NO_MEM);
    
    ESP_ERROR_CHECK(espnow_handler_init(s_forward_queue));
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM));

    int8_t tx_power_qdbm = 0;
    esp_wifi_get_max_tx_power(&tx_power_qdbm);
    ESP_LOGI(TAG, "Potencia TX ESP-NOW Router configurada: raw=%d, %.2f dBm",
            tx_power_qdbm, tx_power_qdbm / 4.0f);

    xTaskCreate(forward_task,"router_forward",4096,NULL,6,NULL);
    xTaskCreate(own_sensor_task,"router_sensor",6144,NULL,5,NULL);
}
