#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZIGBEE_INSTALL_CODE_128_BYTES        18
#define ZIGBEE_INSTALL_CODE_128_HEX_LEN      36

esp_err_t install_code_trust_center_require(bool required);

esp_err_t install_code_trust_center_add_128(const uint8_t ieee_addr[8],
                                            const char *install_code_hex);

esp_err_t install_code_joiner_set_128(const char *install_code_hex);

#ifdef __cplusplus
}
#endif
