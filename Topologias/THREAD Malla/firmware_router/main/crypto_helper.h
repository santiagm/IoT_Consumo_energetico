#ifndef CRYPTO_HELPER_H
#define CRYPTO_HELPER_H
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "config.h"

esp_err_t crypto_make_iv(uint16_t batch_id, const char *src_id, uint8_t iv[AES_GCM_IV_LEN]);
esp_err_t crypto_encrypt_payload(const uint8_t *plain, size_t plain_len,
                                 uint16_t batch_id, const char *src_id,
                                 uint8_t iv[AES_GCM_IV_LEN],
                                 uint8_t *cipher, size_t cipher_capacity,
                                 size_t *cipher_len,
                                 uint8_t tag[AES_GCM_TAG_LEN]);
esp_err_t crypto_decrypt_payload(const uint8_t *cipher, size_t cipher_len,
                                 const uint8_t iv[AES_GCM_IV_LEN],
                                 const uint8_t tag[AES_GCM_TAG_LEN],
                                 uint8_t *plain, size_t plain_capacity,
                                 size_t *plain_len);
#endif
