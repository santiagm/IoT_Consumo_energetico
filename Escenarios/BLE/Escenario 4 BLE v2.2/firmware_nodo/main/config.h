#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include "driver/gpio.h"

/*
 * ============================
 * CONFIGURACION MPU6050
 * ============================
 * Conexion fisica:
 * SDA -> GPIO 6
 * SCL -> GPIO 7
 * INT -> GPIO 4
 */
#define I2C_SDA_GPIO            6
#define I2C_SCL_GPIO            7
#define MPU_INT_GPIO            GPIO_NUM_4

/*
 * Direccion I2C:
 * 0x68 si AD0 esta a GND
 * 0x69 si AD0 esta a VCC
 */
#define MPU_I2C_ADDR            0x69

#define I2C_MASTER_PORT         0
#define I2C_MASTER_FREQ_HZ      400000

/*
 * ============================
 * CONFIGURACION DE MUESTREO
 * ============================
 */
#define STREAM_SAMPLE_COUNT     1

/*
 * MPU6050 en ±2g y ±250 dps.
 * Conversion en Gateway:
 * acelerometro: raw / 16384.0
 * giroscopio:   raw / 131.0
 */
#define MPU_ACCEL_LSB_PER_G     16384.0f
#define MPU_GYRO_LSB_PER_DPS    131.0f

/*
 * ============================
 * CONFIGURACION BLE
 * ============================
 */
#define BLE_COMPANY_ID          0xFFFF

/*
 * Timeout máximo que el nodo permanece anunciando y esperando
 * que el gateway conecte, cifre el enlace y lea la característica GATT segura.
 * Para recuperación usar 5000 ms. Luego optimizar gradualmente.
 */
#define BLE_SEND_TIMEOUT_MS     1500

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
#define NODE_DEBUG_LOGS                 0

#define NODE_ENABLE_RTC_PERIODIC_WAKEUP 1
#define NODE_WAKEUP_PERIOD_SEC          5

/*
 * Para comparación con ESP-NOW Escenario 4:
 * 0 = no despertar por interrupción del MPU6050.
 * El wakeup será únicamente por RTC cada 10 s.
 */
#define NODE_ENABLE_MPU_WAKEUP          0

/*
 * LED RGB integrado ESP32-C6-DevKitC-1
 */
#define RGB_LED_GPIO                    GPIO_NUM_8

#endif