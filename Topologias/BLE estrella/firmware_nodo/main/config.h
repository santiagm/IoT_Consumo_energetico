#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include "driver/gpio.h"

/*
 * ============================
 * CONFIGURACION DE MUESTREO
 * ============================
 */
#define STREAM_SAMPLE_COUNT     1

/*
 * ============================
 * CONFIGURACION BLE
 * ============================
 */
#define BLE_COMPANY_ID          0xFFFF

/* Identidad del nodo dentro de la topologia estrella BLE. */
#define NODE_ID                 "N1"
#define NODE_CRYPTO_ID          1
#define BLE_NODE_DEVICE_NAME    "ESC3_BLE_N1"

/*
 * Timeout máximo que el nodo permanece anunciando y esperando
 * que el gateway conecte, cifre el enlace y lea la característica GATT segura.
 * Para recuperación usar 5000 ms. Luego optimizar gradualmente.
 */
#define BLE_SEND_TIMEOUT_MS     5000

/*
 * Tiempo de gracia despues de que el callback GATT entrega el payload.
 * Evita cortar la conexion antes de que el cliente reciba la respuesta de lectura.
 * No cambia payload, seguridad ni logica BLE; solo retrasa el cierre.
 */
#define BLE_POST_READ_GRACE_MS  100

/*
 * Ventana corta para aproximar un evento de transmisión puntual.
 * Si el gateway pierde paquetes, subir a 500.
 */
#define BLE_ADV_DURATION_MS     300

/*
 * ============================
 * CIFRADO AES-GCM
 * ============================
 */
#define AES_GCM_KEY_LEN         16
#define AES_GCM_IV_LEN          12
#define AES_GCM_TAG_LEN         16

static const uint8_t AES_KEY[16] = {
    'S', 'E', 'C', 'R', 'E', 'T', '_', 'K',
    'E', 'Y', '_', '1', '2', '3', '4', '5'
};

/*
 * ============================
 * ESCENARIO BLE COMPARABLE CON ESP-NOW ESC. 4
 * ============================
 */
#define NODE_DEBUG_LOGS                 1

#define NODE_ENABLE_RTC_PERIODIC_WAKEUP 1
#define NODE_WAKEUP_PERIOD_SEC          5

/*
 * LED RGB integrado ESP32-C6-DevKitC-1
 */
#define RGB_LED_GPIO                    GPIO_NUM_8

#endif
