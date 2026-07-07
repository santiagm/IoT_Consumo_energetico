#ifndef CRYPTO_HELPER_H
#define CRYPTO_HELPER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define AES_GCM_IV_LEN   12
#define AES_GCM_TAG_LEN  16

esp_err_t crypto_decrypt_payload(const uint8_t *ciphertext,
                                 size_t ciphertext_len,
                                 uint16_t batch_id,
                                 uint8_t node_crypto_id,
                                 const uint8_t *auth_tag,
                                 uint8_t *plaintext);

#endif
