#include "crypto_helper.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "psa/crypto.h"

static const char *TAG = "crypto";
static bool s_psa_ready = false;

static esp_err_t crypto_psa_init_once(void)
{
    if (s_psa_ready) {
        return ESP_OK;
    }

    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %ld", (long)st);
        return ESP_FAIL;
    }

    s_psa_ready = true;
    return ESP_OK;
}

esp_err_t crypto_make_iv(uint16_t batch_id, const char *src_id, uint8_t iv[AES_GCM_IV_LEN])
{
    if (!iv) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(iv, 0, AES_GCM_IV_LEN);

    iv[0] = 'T';
    iv[1] = 'H';
    iv[2] = 'R';
    iv[3] = 'D';
    iv[4] = (uint8_t)(batch_id >> 8);
    iv[5] = (uint8_t)(batch_id & 0xFF);

    if (src_id) {
        for (size_t i = 0; src_id[i] != '\0'; ++i) {
            iv[6 + (i % 6)] ^= (uint8_t)src_id[i];
        }
    }

    return ESP_OK;
}

static esp_err_t crypto_import_aes_key(psa_key_id_t *key_id)
{
    if (!key_id) {
        return ESP_ERR_INVALID_ARG;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);

    psa_status_t st = psa_import_key(&attr, AES_KEY, AES_GCM_KEY_LEN, key_id);
    psa_reset_key_attributes(&attr);

    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key failed: %ld", (long)st);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t crypto_encrypt_payload(const uint8_t *plain, size_t plain_len,
                                 uint16_t batch_id, const char *src_id,
                                 uint8_t iv[AES_GCM_IV_LEN],
                                 uint8_t *cipher, size_t cipher_capacity,
                                 size_t *cipher_len,
                                 uint8_t tag[AES_GCM_TAG_LEN])
{
    if (!plain || !iv || !cipher || !cipher_len || !tag) {
        return ESP_ERR_INVALID_ARG;
    }

    if (cipher_capacity < plain_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_ERROR_CHECK(crypto_psa_init_once());
    ESP_ERROR_CHECK(crypto_make_iv(batch_id, src_id, iv));

    psa_key_id_t key_id = 0;
    ESP_ERROR_CHECK(crypto_import_aes_key(&key_id));

    const size_t out_capacity = plain_len + AES_GCM_TAG_LEN;
    uint8_t *out = calloc(1, out_capacity);
    if (!out) {
        psa_destroy_key(key_id);
        return ESP_ERR_NO_MEM;
    }

    size_t out_len = 0;

    psa_status_t st = psa_aead_encrypt(
        key_id,
        PSA_ALG_GCM,
        iv,
        AES_GCM_IV_LEN,
        NULL,
        0,
        plain,
        plain_len,
        out,
        out_capacity,
        &out_len
    );

    psa_destroy_key(key_id);

    if (st != PSA_SUCCESS || out_len != plain_len + AES_GCM_TAG_LEN) {
        ESP_LOGE(TAG, "psa_aead_encrypt failed: %ld out_len=%u",
                 (long)st, (unsigned)out_len);
        free(out);
        return ESP_FAIL;
    }

    memcpy(cipher, out, plain_len);
    memcpy(tag, out + plain_len, AES_GCM_TAG_LEN);
    *cipher_len = plain_len;

    free(out);
    return ESP_OK;
}

esp_err_t crypto_decrypt_payload(const uint8_t *cipher, size_t cipher_len,
                                 const uint8_t iv[AES_GCM_IV_LEN],
                                 const uint8_t tag[AES_GCM_TAG_LEN],
                                 uint8_t *plain, size_t plain_capacity,
                                 size_t *plain_len)
{
    if (!cipher || !iv || !tag || !plain || !plain_len) {
        return ESP_ERR_INVALID_ARG;
    }

    if (plain_capacity < cipher_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_ERROR_CHECK(crypto_psa_init_once());

    psa_key_id_t key_id = 0;
    ESP_ERROR_CHECK(crypto_import_aes_key(&key_id));

    const size_t in_len = cipher_len + AES_GCM_TAG_LEN;
    uint8_t *in = calloc(1, in_len);
    if (!in) {
        psa_destroy_key(key_id);
        return ESP_ERR_NO_MEM;
    }

    memcpy(in, cipher, cipher_len);
    memcpy(in + cipher_len, tag, AES_GCM_TAG_LEN);

    size_t out_len = 0;

    psa_status_t st = psa_aead_decrypt(
        key_id,
        PSA_ALG_GCM,
        iv,
        AES_GCM_IV_LEN,
        NULL,
        0,
        in,
        in_len,
        plain,
        plain_capacity,
        &out_len
    );

    psa_destroy_key(key_id);
    free(in);

    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_aead_decrypt failed/auth error: %ld", (long)st);
        return ESP_FAIL;
    }

    *plain_len = out_len;
    return ESP_OK;
}
