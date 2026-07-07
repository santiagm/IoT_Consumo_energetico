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

/*
 * Timeout máximo que el nodo permanece anunciando y esperando
 * que el gateway conecte y lea la característica GATT.
 * Se conserva el valor optimizado del Escenario 4.
 */
#define BLE_SEND_TIMEOUT_MS     1500

/*
 * Tiempo de gracia despues de que el callback GATT entrega el payload.
 * Se conserva sin cambios respecto al Escenario 4.
 */
#define BLE_POST_READ_GRACE_MS  100

/*
 * Ventana corta para aproximar un evento de transmisión puntual.
 */
#define BLE_ADV_DURATION_MS     300

/*
 * ============================
 * ESCENARIO BLE COMPARABLE CON ESP-NOW ESC. 4
 * ============================
 */
#define NODE_DEBUG_LOGS                 0

#define NODE_ENABLE_RTC_PERIODIC_WAKEUP 1
#define NODE_WAKEUP_PERIOD_SEC          5

/*
 * LED RGB integrado ESP32-C6-DevKitC-1
 */
#define RGB_LED_GPIO                    GPIO_NUM_8

#endif
