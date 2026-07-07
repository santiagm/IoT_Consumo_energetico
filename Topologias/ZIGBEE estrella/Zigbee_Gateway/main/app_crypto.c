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

esp_err_t app_crypto_decrypt_sensor_packet(const secure_zigbee_packet_t *secure_pkt,
                                           sensor_packet_t *plain)
{
    if (secure_pkt == NULL || plain == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (secure_pkt->magic != SECURE_ZB_MAGIC ||
        secure_pkt->version != SECURE_ZB_VERSION ||
        secure_pkt->encrypted_len != SENSOR_PACKET_SIZE) {
        ESP_LOGW(TAG,
                 "Cabecera segura invalida magic=0x%08" PRIx32 " version=%u encrypted_len=%u esperado=%u",
                 secure_pkt->magic,
                 secure_pkt->version,
                 (unsigned)secure_pkt->encrypted_len,
                 (unsigned)SENSOR_PACKET_SIZE);
        return ESP_ERR_INVALID_RESPONSE;
    }

    secure_zigbee_aad_t aad;
    uint8_t cipher_and_tag[SENSOR_PACKET_SIZE + SECURE_ZB_TAG_LEN];
    size_t plain_len = 0;
    psa_key_id_t key_id = 0;

    build_aad(secure_pkt, &aad);
    memset(plain, 0, sizeof(*plain));
    memcpy(cipher_and_tag, secure_pkt->encrypted_payload, SENSOR_PACKET_SIZE);
    memcpy(cipher_and_tag + SENSOR_PACKET_SIZE, secure_pkt->auth_tag, SECURE_ZB_TAG_LEN);

    esp_err_t err = psa_import_aes_gcm_key(&key_id);
    if (err != ESP_OK) {
        return err;
    }

    psa_status_t status = psa_aead_decrypt(key_id,
                                           PSA_ALG_GCM,
                                           secure_pkt->iv,
                                           SECURE_ZB_IV_LEN,
                                           (const uint8_t *)&aad,
                                           sizeof(aad),
                                           cipher_and_tag,
                                           sizeof(cipher_and_tag),
                                           (uint8_t *)plain,
                                           sizeof(*plain),
                                           &plain_len);

    (void)psa_destroy_key(key_id);

    if (status != PSA_SUCCESS) {
        memset(plain, 0, sizeof(*plain));
        ESP_LOGW(TAG, "AES-128-GCM autenticacion fallida batch=%u status=%d",
                 (unsigned)secure_pkt->batch_id,
                 (int)status);
        return ESP_ERR_INVALID_CRC;
    }

    if (plain_len != SENSOR_PACKET_SIZE) {
        memset(plain, 0, sizeof(*plain));
        ESP_LOGW(TAG, "AES-128-GCM longitud descifrada invalida=%u esperado=%u",
                 (unsigned)plain_len,
                 (unsigned)SENSOR_PACKET_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    if (plain->batch_id != secure_pkt->batch_id) {
        ESP_LOGW(TAG,
                 "batch_id descifrado no coincide header=%u plain=%u",
                 (unsigned)secure_pkt->batch_id,
                 (unsigned)plain->batch_id);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG,
             "AES-128-GCM verificado OK batch=%u encrypted_len=%u",
             (unsigned)plain->batch_id,
             (unsigned)secure_pkt->encrypted_len);

    return ESP_OK;
}
