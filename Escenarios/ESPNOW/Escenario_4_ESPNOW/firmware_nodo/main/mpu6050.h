#ifndef MPU6050_H
#define MPU6050_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * @brief Valores de rango para acelerometro del MPU6050.
 */
typedef enum {
    MPU6050_ACCEL_FS_2G = 0,
    MPU6050_ACCEL_FS_4G = 1,
    MPU6050_ACCEL_FS_8G = 2,
    MPU6050_ACCEL_FS_16G = 3,
} mpu6050_accel_fs_t;

/**
 * @brief Valores de rango para giroscopio del MPU6050.
 */
typedef enum {
    MPU6050_GYRO_FS_250DPS = 0,
    MPU6050_GYRO_FS_500DPS = 1,
    MPU6050_GYRO_FS_1000DPS = 2,
    MPU6050_GYRO_FS_2000DPS = 3,
} mpu6050_gyro_fs_t;

/**
 * @brief Configuracion completa y parametrizable del MPU6050.
 */
typedef struct {
    int sda_io;
    int scl_io;
    uint8_t i2c_addr;
    mpu6050_accel_fs_t accel_full_scale;
    mpu6050_gyro_fs_t gyro_full_scale;
    uint16_t sample_rate_hz;
} mpu6050_config_t;

/**
 * @brief Muestra cruda empaquetada (sin conversion) del sensor MPU6050.
 */
typedef struct __attribute__((packed)) {
    int16_t accel_x_raw;
    int16_t accel_y_raw;
    int16_t accel_z_raw;
    int16_t gyro_x_raw;
    int16_t gyro_y_raw;
    int16_t gyro_z_raw;
} mpu6050_sample_t;

/**
 * @brief Inicializa el bus I2C y configura el MPU6050 segun parametros externos.
 *
 * @param config Puntero a la estructura de configuracion del sensor.
 * @return esp_err_t ESP_OK si la inicializacion fue correcta, o un codigo de error en caso contrario.
 */


esp_err_t mpu6050_init(const mpu6050_config_t *config);

esp_err_t mpu6050_sleep(void);
esp_err_t mpu6050_wake(void);
esp_err_t mpu6050_deinit_bus(void);

/**
 * @brief Lee una muestra cruda de acelerometro y giroscopio del MPU6050.
 *
 * @param sample Puntero de salida donde se almacena la muestra cruda.
 * @return esp_err_t ESP_OK si la lectura fue correcta, o un codigo de error en caso contrario.
 */
esp_err_t mpu6050_read_sample(mpu6050_sample_t *sample);

/**
 * @brief Habilita o deshabilita el uso del FIFO para acelerometro + giroscopio.
 *
 * @param enable true para habilitar FIFO, false para deshabilitarlo.
 * @return esp_err_t ESP_OK si la operacion fue correcta, o un codigo de error en caso contrario.
 */
esp_err_t mpu6050_enable_fifo(bool enable);

/**
 * @brief Reinicia el FIFO para descartar muestras antiguas.
 *
 * @return esp_err_t ESP_OK si el reset fue correcto, o un codigo de error en caso contrario.
 */
esp_err_t mpu6050_reset_fifo(void);

/**
 * @brief Obtiene la cantidad de bytes acumulados actualmente en FIFO.
 *
 * @param fifo_count Puntero de salida con el numero de bytes en FIFO.
 * @return esp_err_t ESP_OK si la lectura fue correcta, o un codigo de error en caso contrario.
 */
esp_err_t mpu6050_get_fifo_count(uint16_t *fifo_count);

/**
 * @brief Lee de forma masiva una rafaga de muestras crudas desde FIFO.
 *
 * @param samples Buffer de salida para almacenar las muestras leidas.
 * @param sample_count Cantidad de muestras a leer.
 * @return esp_err_t ESP_OK si la lectura fue correcta, o un codigo de error en caso contrario.
 */
esp_err_t mpu6050_read_fifo_burst(mpu6050_sample_t *samples, size_t sample_count);

/**
 * @brief Configura la deteccion de movimiento (WOM) del MPU6050.
 *
 * @param threshold Umbral de movimiento (registro MOT_THR).
 * @param duration Duracion minima del movimiento (registro MOT_DUR).
 * @return esp_err_t ESP_OK si la configuracion fue correcta, o un codigo de error en caso contrario.
 */
esp_err_t mpu6050_setup_motion_interrupt(uint8_t threshold, uint8_t duration);

/**
 * @brief Limpia la interrupcion pendiente leyendo el registro INT_STATUS.
 *
 * @return esp_err_t ESP_OK si la lectura fue correcta, o un codigo de error en caso contrario.
 */
esp_err_t mpu6050_clear_interrupt_status(void);

#endif
