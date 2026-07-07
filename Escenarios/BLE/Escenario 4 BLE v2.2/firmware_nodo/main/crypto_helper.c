#include "crypto_helper.h"
#include "config.h"

#include <string.h>

#include "esp_log.h"
#include "psa/crypto.h"

static const char *TAG = "CRYPTO_HELPER";

static void build_iv(uint16_t batch_id, uint8_t iv[AES_GCM_IV_LEN])
{
    memset(iv, 0, AES_GCM_IV_LEN);
    iv[0] = (uint8_t)(batch_id & 0xFF);
    iv[1] = (uint8_t)((batch_id >> 8) & 0xFF);
}

esp_err_t crypto_encrypt_payload(const uint8_t *plaintext,
                                 size_t plaintext_len,
                                 uint16_t batch_id,
                                 uint8_t *ciphertext,
                                 uint8_t *auth_tag)
{
    if (plaintext == NULL || ciphertext == NULL || auth_tag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t iv[AES_GCM_IV_LEN];
    build_iv(batch_id, iv);

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init fallo: %d", (int)status);
        return ESP_FAIL;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;

    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, AES_GCM_KEY_LEN * 8);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);

    status = psa_import_key(&attributes,
                            AES_KEY,
                            AES_GCM_KEY_LEN,
                            &key_id);

    psa_reset_key_attributes(&attributes);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key fallo: %d", (int)status);
        return ESP_FAIL;
    }

    uint8_t output[plaintext_len + AES_GCM_TAG_LEN];
    size_t output_len = 0;

    status = psa_aead_encrypt(key_id,
                              PSA_ALG_GCM,
                              iv,
                              AES_GCM_IV_LEN,
                              NULL,
                              0,
                              plaintext,
                              plaintext_len,
                              output,
                              sizeof(output),
                              &output_len);

    psa_destroy_key(key_id);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_aead_encrypt fallo: %d", (int)status);
        return ESP_FAIL;
    }

    if (output_len != plaintext_len + AES_GCM_TAG_LEN) {
        ESP_LOGE(TAG, "AES-GCM longitud inesperada: %u", (unsigned)output_len);
        return ESP_FAIL;
    }

    memcpy(ciphertext, output, plaintext_len);
    memcpy(auth_tag, output + plaintext_len, AES_GCM_TAG_LEN);

    return ESP_OK;
}