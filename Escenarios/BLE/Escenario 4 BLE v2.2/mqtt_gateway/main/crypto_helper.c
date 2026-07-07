#include "crypto_helper.h"
#include "config.h"

#include "esp_log.h"
#include "psa/crypto.h"

#include <stdbool.h>
#include <string.h>

static const char *TAG = "CRYPTO";
static bool s_psa_initialized = false;

esp_err_t crypto_decrypt_payload(const uint8_t *ciphertext,
                                 size_t ciphertext_len,
                                 uint16_t batch_id,
                                 const uint8_t *auth_tag,
                                 uint8_t *plaintext)
{
    if (ciphertext == NULL || auth_tag == NULL || plaintext == NULL) {
        ESP_LOGE(TAG, "Parametro NULL en crypto_decrypt_payload");
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_psa_initialized) {
        psa_status_t init_status = psa_crypto_init();
        if (init_status != PSA_SUCCESS) {
            ESP_LOGE(TAG, "psa_crypto_init fallo: %d", (int)init_status);
            return ESP_FAIL;
        }
        s_psa_initialized = true;
    }

    uint8_t iv[AES_GCM_IV_LEN] = {0};
    memcpy(iv, &batch_id, sizeof(batch_id));

    const size_t combined_len = ciphertext_len + AES_GCM_TAG_LEN;
    uint8_t combined[256];

    if (combined_len > sizeof(combined)) {
        ESP_LOGE(TAG, "Paquete cifrado demasiado grande: %u", (unsigned)combined_len);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(combined, ciphertext, ciphertext_len);
    memcpy(combined + ciphertext_len, auth_tag, AES_GCM_TAG_LEN);

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;

    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);

    psa_status_t status = psa_import_key(&attributes,
                                         AES_KEY,
                                         sizeof(AES_KEY),
                                         &key_id);

    psa_reset_key_attributes(&attributes);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key fallo: %d", (int)status);
        return ESP_FAIL;
    }

    size_t plaintext_len = 0;

    status = psa_aead_decrypt(key_id,
                              PSA_ALG_GCM,
                              iv,
                              AES_GCM_IV_LEN,
                              NULL,
                              0,
                              combined,
                              combined_len,
                              plaintext,
                              ciphertext_len,
                              &plaintext_len);

    psa_destroy_key(key_id);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Fallo autenticacion/descifrado AES-GCM: %d", (int)status);
        return ESP_FAIL;
    }

    if (plaintext_len != ciphertext_len) {
        ESP_LOGW(TAG, "Longitud descifrada inesperada: %u vs %u",
                 (unsigned)plaintext_len,
                 (unsigned)ciphertext_len);
    }

    return ESP_OK;
}
