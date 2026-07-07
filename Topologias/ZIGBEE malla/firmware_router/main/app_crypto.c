#include <string.h>
#include <inttypes.h>

#include "app_crypto.h"
#include "config.h"

#include "esp_log.h"
#include "psa/crypto.h"

static const char *TAG = "APP_AES_GCM";

static void build_aad(const secure_zigbee_packet_t *secure_pkt, secure_zigbee_aad_t *aad)
{
    aad->magic = secure_pkt->magic;
    aad->version = secure_pkt->version;
    aad->batch_id = secure_pkt->batch_id;
    aad->encrypted_len = secure_pkt->encrypted_len;
}

static void make_iv(const sensor_packet_t *plain, uint8_t iv[SECURE_ZB_IV_LEN])
{
    memset(iv, 0, SECURE_ZB_IV_LEN);
    iv[0] = 'Z';
    iv[1] = 'B';
    iv[2] = 'G';
    iv[3] = '4';
    iv[4] = (uint8_t)(plain->batch_id & 0xFF);
    iv[5] = (uint8_t)((plain->batch_id >> 8) & 0xFF);
    iv[6] = (uint8_t)(plain->start_timestamp_ms & 0xFF);
    iv[7] = (uint8_t)((plain->start_timestamp_ms >> 8) & 0xFF);
    iv[8] = (uint8_t)((plain->start_timestamp_ms >> 16) & 0xFF);
    iv[9] = (uint8_t)((plain->start_timestamp_ms >> 24) & 0xFF);
    /* Ultimos dos bytes variables desde src_mac XX:YY. */
    iv[10] = (uint8_t)plain->src_mac[15];
    iv[11] = (uint8_t)plain->src_mac[16];
}

static esp_err_t psa_import_aes_gcm_key(psa_key_id_t *key_id)
{
    static const uint8_t key[16] = APP_AES_128_GCM_KEY_BYTES;

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS && status != PSA_ERROR_BAD_STATE) {
        ESP_LOGE(TAG, "psa_crypto_init fallo status=%d", (int)status);
        return ESP_FAIL;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);

    status = psa_import_key(&attributes, key, sizeof(key), key_id);
    psa_reset_key_attributes(&attributes);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key fallo status=%d", (int)status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t app_crypto_encrypt_sensor_packet(const sensor_packet_t *plain,
                                           secure_zigbee_packet_t *secure_pkt)
{
    if (plain == NULL || secure_pkt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    secure_zigbee_aad_t aad;
    uint8_t cipher_and_tag[SENSOR_PACKET_SIZE + SECURE_ZB_TAG_LEN];
    size_t cipher_and_tag_len = 0;
    psa_key_id_t key_id = 0;

    memset(secure_pkt, 0, sizeof(*secure_pkt));
    secure_pkt->magic = SECURE_ZB_MAGIC;
    secure_pkt->version = SECURE_ZB_VERSION;
    secure_pkt->batch_id = plain->batch_id;
    secure_pkt->encrypted_len = SENSOR_PACKET_SIZE;
    make_iv(plain, secure_pkt->iv);
    build_aad(secure_pkt, &aad);

    esp_err_t err = psa_import_aes_gcm_key(&key_id);
    if (err != ESP_OK) {
        return err;
    }

    psa_status_t status = psa_aead_encrypt(key_id,
                                           PSA_ALG_GCM,
                                           secure_pkt->iv,
                                           SECURE_ZB_IV_LEN,
                                           (const uint8_t *)&aad,
                                           sizeof(aad),
                                           (const uint8_t *)plain,
                                           SENSOR_PACKET_SIZE,
                                           cipher_and_tag,
                                           sizeof(cipher_and_tag),
                                           &cipher_and_tag_len);

    (void)psa_destroy_key(key_id);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "AES-128-GCM cifrado PSA fallo status=%d", (int)status);
        return ESP_FAIL;
    }

    if (cipher_and_tag_len != (SENSOR_PACKET_SIZE + SECURE_ZB_TAG_LEN)) {
        ESP_LOGE(TAG, "AES-128-GCM longitud inesperada out=%u esperado=%u",
                 (unsigned)cipher_and_tag_len,
                 (unsigned)(SENSOR_PACKET_SIZE + SECURE_ZB_TAG_LEN));
        return ESP_FAIL;
    }

    memcpy(secure_pkt->encrypted_payload, cipher_and_tag, SENSOR_PACKET_SIZE);
    memcpy(secure_pkt->auth_tag, cipher_and_tag + SENSOR_PACKET_SIZE, SECURE_ZB_TAG_LEN);

    ESP_LOGI(TAG,
             "AES-128-GCM OK batch=%u plain_len=%u secure_len=%u iv=%02X%02X%02X%02X... tag=%02X%02X...",
             (unsigned)plain->batch_id,
             (unsigned)SENSOR_PACKET_SIZE,
             (unsigned)sizeof(secure_zigbee_packet_t),
             secure_pkt->iv[0], secure_pkt->iv[1], secure_pkt->iv[2], secure_pkt->iv[3],
             secure_pkt->auth_tag[0], secure_pkt->auth_tag[1]);

    return ESP_OK;
}
