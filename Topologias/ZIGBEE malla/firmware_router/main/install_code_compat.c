#include "install_code_compat.h"

#include "esp_log.h"
#include "esp_err.h"

#include "ezbee/secur.h"

#include <ctype.h>
#include <string.h>

static const char *TAG = "IC_COMPAT";

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }

    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }

    return -1;
}

static esp_err_t hex_to_bytes_exact(const char *hex,
                                    uint8_t *out,
                                    size_t out_len)
{
    if (hex == NULL || out == NULL) {
        ESP_LOGE(TAG, "hex_to_bytes_exact: argumento NULL");
        return ESP_ERR_INVALID_ARG;
    }

    size_t hex_len = strlen(hex);

    if (hex_len != out_len * 2) {
        ESP_LOGE(TAG,
                 "Install Code longitud incorrecta hex_len=%u esperado=%u",
                 (unsigned)hex_len,
                 (unsigned)(out_len * 2));
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);

        if (hi < 0 || lo < 0) {
            ESP_LOGE(TAG,
                     "Install Code contiene caracter no hexadecimal en byte=%u",
                     (unsigned)i);
            return ESP_ERR_INVALID_ARG;
        }

        out[i] = (uint8_t)((hi << 4) | lo);
    }

    return ESP_OK;
}

static void log_ieee(const uint8_t ieee[8])
{
    ESP_LOGI(TAG,
             "IEEE autorizado raw=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             ieee[0],
             ieee[1],
             ieee[2],
             ieee[3],
             ieee[4],
             ieee[5],
             ieee[6],
             ieee[7]);
}

static void log_install_code(const uint8_t ic[ZIGBEE_INSTALL_CODE_128_BYTES])
{
    ESP_LOGI(TAG,
             "Install Code bytes=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X CRC=%02X %02X",
             ic[0],
             ic[1],
             ic[2],
             ic[3],
             ic[4],
             ic[5],
             ic[6],
             ic[7],
             ic[8],
             ic[9],
             ic[10],
             ic[11],
             ic[12],
             ic[13],
             ic[14],
             ic[15],
             ic[16],
             ic[17]);
}

esp_err_t install_code_trust_center_require(bool required)
{
    ezb_err_t zb_err = ezb_secur_set_ic_required(required);

    if (zb_err != EZB_ERR_NONE) {
        ESP_LOGE(TAG,
                 "ezb_secur_set_ic_required fallo err=%d",
                 (int)zb_err);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Install Code requerido habilitado=%d", required ? 1 : 0);

    return ESP_OK;
}

esp_err_t install_code_trust_center_add_128(const uint8_t ieee_addr[8],
                                            const char *install_code_hex)
{
    if (ieee_addr == NULL || install_code_hex == NULL) {
        ESP_LOGE(TAG, "install_code_trust_center_add_128: argumento NULL");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t ic[ZIGBEE_INSTALL_CODE_128_BYTES];

    esp_err_t err = hex_to_bytes_exact(install_code_hex,
                                       ic,
                                       sizeof(ic));

    if (err != ESP_OK) {
        return err;
    }

    /*
     * MUY IMPORTANTE:
     * La API oficial usa const ezb_extaddr_t *address.
     * No se debe pasar directamente uint8_t * sin crear ezb_extaddr_t.
     */
    ezb_extaddr_t ext_addr = {0};

    memcpy(ext_addr.u8, ieee_addr, 8);

    log_ieee(ieee_addr);
    log_install_code(ic);

    ezb_err_t zb_err = ezb_secur_ic_add(&ext_addr,
                                        EZB_SECUR_IC_TYPE_128,
                                        ic);

    if (zb_err != EZB_ERR_NONE) {
        ESP_LOGE(TAG,
                 "ezb_secur_ic_add fallo err=%d. Posibles causas: IEEE no valido, CRC no valido, formato no valido o tipo IC incorrecto.",
                 (int)zb_err);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Install Code del nodo registrado correctamente");

    return ESP_OK;
}

esp_err_t install_code_joiner_set_128(const char *install_code_hex)
{
    if (install_code_hex == NULL) {
        ESP_LOGE(TAG, "install_code_joiner_set_128: argumento NULL");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t ic[ZIGBEE_INSTALL_CODE_128_BYTES];

    esp_err_t err = hex_to_bytes_exact(install_code_hex,
                                       ic,
                                       sizeof(ic));

    if (err != ESP_OK) {
        return err;
    }

    log_install_code(ic);

    ezb_err_t zb_err = ezb_secur_ic_set(EZB_SECUR_IC_TYPE_128, ic);

    if (zb_err != EZB_ERR_NONE) {
        ESP_LOGE(TAG,
                 "ezb_secur_ic_set fallo err=%d",
                 (int)zb_err);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Install Code configurado en joiner correctamente");

    return ESP_OK;
}
