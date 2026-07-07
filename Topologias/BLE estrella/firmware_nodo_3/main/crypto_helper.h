#ifndef CRYPTO_HELPER_H
#define CRYPTO_HELPER_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t crypto_encrypt_payload(const uint8_t *plaintext,
                                 size_t plaintext_len,
                                 uint16_t batch_id,
                                 uint8_t node_crypto_id,
                                 uint8_t *ciphertext,
                                 uint8_t *auth_tag);

#endif
