#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include "driver/gpio.h"

/*
 * BLE malla simulada isofuncional con BLE P2P/estrella:
 * N1/N2 -> N3 relay -> Gateway, usando BLE GATT seguro en ambos saltos.
 * N3 NO descifra AES-GCM de aplicacion; solo reenvia el payload cifrado.
 */

#define STREAM_SAMPLE_COUNT     1

#define BLE_COMPANY_ID          0xFFFF

#define NODE_ID                 "N3"
#define NODE_CRYPTO_ID          3
#define BLE_RELAY_DEVICE_NAME   "ESC3_BLE_N3"
#define BLE_CHILD_NAME_N1       "ESC3_BLE_N1"
#define BLE_CHILD_NAME_N2       "ESC3_BLE_N2"
#define BLE_CHILD_NAME_PREFIX   "ESC3_BLE_N"

/* Mismos tiempos base usados en BLE P2P/estrella. */
#define BLE_SEND_TIMEOUT_MS     1500
#define BLE_POST_READ_GRACE_MS  100
#define BLE_ADV_DURATION_MS     300

/* Mismos parametros de escaneo usados en BLE estrella. */
#define BLE_SCAN_INTERVAL       0x0030
#define BLE_SCAN_WINDOW         0x0030

#define AES_GCM_KEY_LEN         16
#define AES_GCM_IV_LEN          12
#define AES_GCM_TAG_LEN         16

/* Misma llave de aplicacion que P2P/estrella; N3 la conserva solo por trazabilidad.
 * El relay no llama a AES-GCM ni autentica el payload. */
static const uint8_t AES_KEY[16] = {
    'S', 'E', 'C', 'R', 'E', 'T', '_', 'K',
    'E', 'Y', '_', '1', '2', '3', '4', '5'
};

#define NODE_DEBUG_LOGS                 1
#define NODE_ENABLE_RTC_PERIODIC_WAKEUP 0
#define NODE_WAKEUP_PERIOD_SEC          5

#define RGB_LED_GPIO                    GPIO_NUM_8

#endif
