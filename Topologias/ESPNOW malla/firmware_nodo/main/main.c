#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "psa/crypto.h"
#include "config.h"
#include "espnow_handler.h"
#include "mesh_payload.h"

static const char *TAG = "NODO_" NODE_ID;
RTC_DATA_ATTR static uint32_t s_packet_id;
RTC_DATA_ATTR static uint16_t s_batch_id;
static const uint8_t s_key[16] = APP_AES_GCM_KEY_BYTES;

/* N1/N2 cifran el paquete completo; N3 solo modifica la cabecera mesh externa. */

static void mac_text(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

static void fill_sample(mpu_sample_t *s, uint32_t seed)
{
    s->ax = (int16_t)(120 + (seed & 63));
    s->ay = (int16_t)(-80 + ((seed >> 6) & 63));
    s->az = (int16_t)(16384 + ((seed >> 12) & 127));
    s->gx = (int16_t)(5 + ((seed >> 19) & 31));
    s->gy = (int16_t)(-3 + ((seed >> 24) & 31));
    s->gz = (int16_t)(2 + ((seed >> 29) & 7));
}

static esp_err_t encrypt_packet(const sensor_packet_t *plain, secure_espnow_packet_t *secure)
{
    memset(secure, 0, sizeof(*secure));
    secure->magic = ESPNOW_APP_SEC_MAGIC;
    secure->version = ESPNOW_APP_SEC_VERSION;
    secure->batch_id = plain->batch_id;
    secure->encrypted_len = sizeof(*plain);
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, mac), TAG, "read MAC");
    secure->iv[0]='E'; secure->iv[1]='N'; memcpy(&secure->iv[2], &mac[2], 4);
    memcpy(&secure->iv[6], &plain->batch_id, 2);
    memcpy(&secure->iv[8], &plain->node_tx_timestamp_ms, 4);
    secure_packet_aad_t aad = {secure->magic, secure->version, secure->batch_id, secure->encrypted_len, {0}};
    memcpy(aad.iv, secure->iv, AES_GCM_IV_LEN);
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key = 0;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES); psa_set_key_bits(&attr, 128);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT); psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    if (psa_crypto_init()!=PSA_SUCCESS || psa_import_key(&attr,s_key,sizeof(s_key),&key)!=PSA_SUCCESS) return ESP_FAIL;
    uint8_t output[sizeof(*plain)+AES_GCM_TAG_LEN]; size_t output_len=0;
    psa_status_t st=psa_aead_encrypt(key,PSA_ALG_GCM,secure->iv,AES_GCM_IV_LEN,(uint8_t*)&aad,sizeof(aad),(uint8_t*)plain,sizeof(*plain),output,sizeof(output),&output_len);
    psa_destroy_key(key); psa_reset_key_attributes(&attr);
    if (st!=PSA_SUCCESS || output_len!=sizeof(output)) return ESP_FAIL;
    memcpy(secure->encrypted_payload,output,sizeof(*plain));
    memcpy(secure->auth_tag,output+sizeof(*plain),AES_GCM_TAG_LEN);
    return ESP_OK;
}

void app_main(void)
{
    /* Un ciclo de trabajo: radio, muestra, envio confirmado y deep sleep. */
    ESP_ERROR_CHECK(espnow_handler_init());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM));
    int8_t tx_power_qdbm = 0;
    esp_wifi_get_max_tx_power(&tx_power_qdbm);
    ESP_LOGI(TAG, "Potencia TX ESP-NOW configurada: raw=%d, %.2f dBm",
            tx_power_qdbm, tx_power_qdbm / 4.0f);
    uint8_t mac[6]; ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    ESP_LOGI(TAG, "MAC STA: %02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

    sensor_packet_t packet = {0};
    strlcpy(packet.node_id, NODE_ID, sizeof(packet.node_id));
    mac_text(mac, packet.src_mac);
    packet.packet_id = ++s_packet_id;
    packet.batch_id = ++s_batch_id;
    packet.node_tx_timestamp_ms = (uint32_t)(esp_timer_get_time()/1000ULL);
    for (size_t i=0;i<STREAM_SAMPLE_COUNT;i++) fill_sample(&packet.muestras[i],esp_random());
    secure_espnow_packet_t secure;
    espnow_mesh_frame_t frame = {0};
    ESP_ERROR_CHECK(encrypt_packet(&packet,&secure));
    frame.mesh_magic=ESPNOW_MESH_MAGIC; frame.mesh_version=ESPNOW_MESH_VERSION; frame.mesh_type=ESPNOW_MESH_TYPE_DATA;
    strlcpy(frame.origin_node_id,NODE_ID,sizeof(frame.origin_node_id)); memcpy(frame.origin_mac,mac,6);
    frame.hop_count=0; frame.first_hop_rssi=MESH_RSSI_UNKNOWN; frame.last_hop_rssi=MESH_RSSI_UNKNOWN;
    frame.packet_id_shadow=packet.packet_id; frame.batch_id_shadow=packet.batch_id; frame.secure_len=sizeof(secure);
    memcpy(frame.secure_payload,&secure,sizeof(secure));
    ESP_LOGI(TAG,"Enviando paquete node_id=%s packet_id=%"PRIu32" batch_id=%u hacia ROUTER N3",NODE_ID,packet.packet_id,packet.batch_id);
    ESP_ERROR_CHECK(espnow_handler_send((uint8_t*)&frame,sizeof(frame)));
    ESP_LOGI(TAG,"Entrando a deep sleep");
    esp_now_deinit(); esp_wifi_stop();
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(RTC_WAKEUP_TIME_US));
    esp_deep_sleep_start();
}
