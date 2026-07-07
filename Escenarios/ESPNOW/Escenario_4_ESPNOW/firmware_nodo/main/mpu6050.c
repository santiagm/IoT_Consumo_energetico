
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"

#include "mpu6050.h"

#define MPU6050_DEFAULT_ADDR            0x69

#define MPU6050_REG_SMPLRT_DIV          0x19
#define MPU6050_REG_CONFIG              0x1A
#define MPU6050_REG_GYRO_CONFIG         0x1B
#define MPU6050_REG_ACCEL_CONFIG        0x1C
#define MPU6050_REG_MOT_THR             0x1F
#define MPU6050_REG_MOT_DUR             0x20
#define MPU6050_REG_FIFO_EN             0x23
#define MPU6050_REG_INT_PIN_CFG         0x37
#define MPU6050_REG_INT_ENABLE          0x38
#define MPU6050_REG_INT_STATUS          0x3A
#define MPU6050_REG_ACCEL_XOUT_H        0x3B
#define MPU6050_REG_USER_CTRL           0x6A
#define MPU6050_REG_PWR_MGMT_1          0x6B
#define MPU6050_REG_PWR_MGMT_2          0x6C
#define MPU6050_REG_FIFO_COUNTH         0x72
#define MPU6050_REG_FIFO_R_W            0x74
#define MPU6050_REG_WHO_AM_I            0x75

#define MPU6050_WHO_AM_I_EXPECTED       0x68

#define MPU6050_FIFO_PACKET_BYTES       12U

static const char *TAG = "MPU6050";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_mpu_dev = NULL;
static mpu6050_config_t s_cfg = {
    .sda_io = 6,
    .scl_io = 7,
    .i2c_addr = MPU6050_DEFAULT_ADDR,
    .accel_full_scale = MPU6050_ACCEL_FS_2G,
    .gyro_full_scale = MPU6050_GYRO_FS_250DPS,
    .sample_rate_hz = 100,
};

/**
 * @brief Escribe un byte en un registro del MPU6050.
 *
 * @param reg Direccion del registro.
 * @param value Valor de un byte a escribir.
 * @return esp_err_t ESP_OK si la operacion fue correcta, o un error de transferencia I2C.
 */
static esp_err_t mpu6050_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(s_mpu_dev, payload, sizeof(payload), 100);
}

/**
 * @brief Lee uno o mas bytes desde un registro del MPU6050.
 *
 * @param reg Registro inicial a leer.
 * @param data Buffer de salida para almacenar los datos leidos.
 * @param len Cantidad de bytes a leer.
 * @return esp_err_t ESP_OK si la lectura fue correcta, o un error de transferencia I2C.
 */
static esp_err_t mpu6050_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_mpu_dev, &reg, 1, data, len, 100);
}

/**
 * @brief Lee-modifica-escribe un registro aplicando mascara y valor.
 *
 * @param reg Registro a modificar.
 * @param mask Mascara de bits a actualizar.
 * @param value Valor con los bits finales ya alineados.
 * @return esp_err_t ESP_OK si la operacion fue correcta, o un codigo de error en caso contrario.
 */
static esp_err_t mpu6050_update_reg_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
    esp_err_t err;
    uint8_t current = 0;

    err = mpu6050_read_regs(reg, &current, 1);
    if (err != ESP_OK) {
        return err;
    }

    current = (uint8_t)((current & (~mask)) | (value & mask));
    return mpu6050_write_reg(reg, current);
}

/**
 * @brief Coloca el MPU6050 en modo sleep.
 *
 * En este modo el sensor reduce su consumo, pero no podrá generar
 * interrupciones de movimiento para despertar al ESP32.
 *
 * PWR_MGMT_1 bit 6 = SLEEP.
 */
esp_err_t mpu6050_sleep(void)
{
    if (s_mpu_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err;
    uint8_t pwr_mgmt_1 = 0;
    uint8_t pwr_mgmt_2 = 0;

    /*
     * Secuencia reforzada para reducir consumo del MPU6050 antes de que
     * el ESP32-C6 entre en deep sleep.
     */

    /* 1) Deshabilitar interrupciones del MPU6050. */
    err = mpu6050_write_reg(MPU6050_REG_INT_ENABLE, 0x00);
    if (err != ESP_OK) {
        return err;
    }

    /* 2) Deshabilitar FIFO y sensores en FIFO. */
    err = mpu6050_write_reg(MPU6050_REG_FIFO_EN, 0x00);
    if (err != ESP_OK) {
        return err;
    }

    err = mpu6050_update_reg_bits(MPU6050_REG_USER_CTRL, 0x40, 0x00);
    if (err != ESP_OK) {
        return err;
    }

    /* 3) Resetear FIFO para que no quede actividad interna pendiente. */
    err = mpu6050_update_reg_bits(MPU6050_REG_USER_CTRL, 0x04, 0x04);
    if (err != ESP_OK) {
        return err;
    }

    /* 4) Limpiar interrupciones pendientes leyendo INT_STATUS. */
    (void)mpu6050_clear_interrupt_status();

    /*
     * 5) Poner todos los ejes de acelerometro y giroscopio en standby.
     * PWR_MGMT_2 bits 5:0 = 1 apaga XA, YA, ZA, XG, YG, ZG.
     */
    err = mpu6050_write_reg(MPU6050_REG_PWR_MGMT_2, 0x3F);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * 6) Activar modo sleep del chip.
     * PWR_MGMT_1 bit 6 = SLEEP.
     */
    err = mpu6050_write_reg(MPU6050_REG_PWR_MGMT_1, 0x40);
    if (err != ESP_OK) {
        return err;
    }

    /* 7) Verificacion por lectura para confirmar que el registro quedo escrito. */
    err = mpu6050_read_regs(MPU6050_REG_PWR_MGMT_1, &pwr_mgmt_1, 1);
    if (err != ESP_OK) {
        return err;
    }

    err = mpu6050_read_regs(MPU6050_REG_PWR_MGMT_2, &pwr_mgmt_2, 1);
    if (err != ESP_OK) {
        return err;
    }

    if ((pwr_mgmt_1 & 0x40) == 0) {
        ESP_LOGW(TAG, "MPU6050 sleep no confirmado: PWR_MGMT_1=0x%02X", pwr_mgmt_1);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG,
             "MPU6050 sleep confirmado: PWR_MGMT_1=0x%02X PWR_MGMT_2=0x%02X",
             pwr_mgmt_1,
             pwr_mgmt_2);

    return ESP_OK;
}

/**
 * @brief Despierta el MPU6050 desde modo sleep.
 *
 * Se usa para volver a dejar el sensor activo antes de medir.
 */
esp_err_t mpu6050_wake(void)
{
    if (s_mpu_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err;

    /* Salir de sleep. PWR_MGMT_1 = 0x00 usa reloj interno. */
    err = mpu6050_write_reg(MPU6050_REG_PWR_MGMT_1, 0x00);
    if (err != ESP_OK) {
        return err;
    }

    /* Reactivar todos los ejes de acelerometro y giroscopio. */
    err = mpu6050_write_reg(MPU6050_REG_PWR_MGMT_2, 0x00);
    if (err != ESP_OK) {
        return err;
    }

    /* Tiempo recomendado para estabilizacion del sensor despues de wake. */
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "MPU6050 despierto desde modo sleep");

    return ESP_OK;
}

esp_err_t mpu6050_deinit_bus(void)
{
    esp_err_t err;

    if (s_mpu_dev != NULL) {
        err = i2c_master_bus_rm_device(s_mpu_dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "No se pudo remover dispositivo I2C MPU6050: %s", esp_err_to_name(err));
            return err;
        }
        s_mpu_dev = NULL;
    }

    if (s_i2c_bus != NULL) {
        err = i2c_del_master_bus(s_i2c_bus);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "No se pudo eliminar bus I2C: %s", esp_err_to_name(err));
            return err;
        }
        s_i2c_bus = NULL;
    }

    ESP_LOGI(TAG, "Bus I2C liberado antes de deep sleep");
    return ESP_OK;
}

/**
 * @brief Inicializa el bus I2C maestro con la configuracion actual.
 *
 * @return esp_err_t ESP_OK en caso de exito, o un error del driver I2C.
 */
static esp_err_t init_i2c_bus(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = 0,
        .sda_io_num = s_cfg.sda_io,
        .scl_io_num = s_cfg.scl_io,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
            .allow_pd = 0,
        },
    };

    return i2c_new_master_bus(&bus_config, &s_i2c_bus);
}

/**
 * @brief Configura el divisor de muestreo para aproximar sample_rate_hz.
 *
 * @return esp_err_t ESP_OK si la configuracion fue correcta, o un codigo de error.
 */
static esp_err_t mpu6050_apply_sample_rate(void)
{
    uint16_t target_hz = s_cfg.sample_rate_hz;
    uint8_t divider;

    if (target_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (target_hz > 1000) {
        target_hz = 1000;
    }

    if (target_hz < 4) {
        target_hz = 4;
    }

    divider = (uint8_t)((1000U / target_hz) - 1U);

    /* DLPF=3 (44Hz) estabiliza el stream y mantiene base de 1kHz para SMPLRT_DIV. */
    if (mpu6050_write_reg(MPU6050_REG_CONFIG, 0x03) != ESP_OK) {
        return ESP_FAIL;
    }

    return mpu6050_write_reg(MPU6050_REG_SMPLRT_DIV, divider);
}

/**
 * @brief Inicializa el bus I2C y configura el MPU6050 segun parametros externos.
 *
 * @param config Puntero a la estructura de configuracion del sensor.
 * @return esp_err_t ESP_OK si la inicializacion fue correcta, o un codigo de error en caso contrario.
 */
esp_err_t mpu6050_init(const mpu6050_config_t *config)
{
    esp_err_t err;
    uint8_t who_am_i = 0;
    i2c_device_config_t dev_cfg;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = *config;

    if (s_cfg.i2c_addr == 0) {
        s_cfg.i2c_addr = MPU6050_DEFAULT_ADDR;
    }

    if (s_i2c_bus == NULL) {
        err = init_i2c_bus();
        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "No se pudo inicializar I2C en SDA=%d SCL=%d: %s",
                     s_cfg.sda_io,
                     s_cfg.scl_io,
                     esp_err_to_name(err));
            return err;
        }
    }

    if (s_mpu_dev == NULL) {
        memset(&dev_cfg, 0, sizeof(dev_cfg));
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = s_cfg.i2c_addr;
        dev_cfg.scl_speed_hz = 400000;
        dev_cfg.scl_wait_us = 0;
        dev_cfg.flags.disable_ack_check = 0;

        err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_mpu_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo registrar dispositivo I2C MPU6050: %s", esp_err_to_name(err));
            return err;
        }
    }

    err = mpu6050_read_regs(MPU6050_REG_WHO_AM_I, &who_am_i, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo leer WHO_AM_I del MPU6050: %s", esp_err_to_name(err));
        return err;
    }

    if (who_am_i != MPU6050_WHO_AM_I_EXPECTED) {
        ESP_LOGE(TAG,
                 "WHO_AM_I invalido. Esperado=0x%02X recibido=0x%02X",
                 MPU6050_WHO_AM_I_EXPECTED,
                 who_am_i);
        return ESP_ERR_INVALID_RESPONSE;
    }

    err = mpu6050_write_reg(MPU6050_REG_PWR_MGMT_1, 0x00);
    if (err != ESP_OK) {
        return err;
    }

    err = mpu6050_apply_sample_rate();
    if (err != ESP_OK) {
        return err;
    }

    err = mpu6050_update_reg_bits(MPU6050_REG_ACCEL_CONFIG,
                                  0x18,
                                  (uint8_t)((s_cfg.accel_full_scale & 0x03) << 3));
    if (err != ESP_OK) {
        return err;
    }

    err = mpu6050_update_reg_bits(MPU6050_REG_GYRO_CONFIG,
                                  0x18,
                                  (uint8_t)((s_cfg.gyro_full_scale & 0x03) << 3));
    if (err != ESP_OK) {
        return err;
    }

    /* Se deja FIFO deshabilitado por defecto hasta que la aplicacion lo solicite. */
    err = mpu6050_enable_fifo(false);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG,
             "MPU6050 inicializado: addr=0x%02X fs_acc=%u fs_gyro=%u fs_hz=%u",
             s_cfg.i2c_addr,
             (unsigned)s_cfg.accel_full_scale,
             (unsigned)s_cfg.gyro_full_scale,
             (unsigned)s_cfg.sample_rate_hz);

    return ESP_OK;
}

/**
 * @brief Lee una muestra cruda de acelerometro y giroscopio del MPU6050.
 *
 * @param sample Puntero de salida donde se almacena la muestra cruda.
 * @return esp_err_t ESP_OK si la lectura fue correcta, o un codigo de error en caso contrario.
 */
esp_err_t mpu6050_read_sample(mpu6050_sample_t *sample)
{
    uint8_t raw[14] = {0};
    esp_err_t err;

    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mpu_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    err = mpu6050_read_regs(MPU6050_REG_ACCEL_XOUT_H, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    sample->accel_x_raw = (int16_t)((raw[0] << 8) | raw[1]);
    sample->accel_y_raw = (int16_t)((raw[2] << 8) | raw[3]);
    sample->accel_z_raw = (int16_t)((raw[4] << 8) | raw[5]);

    sample->gyro_x_raw = (int16_t)((raw[8] << 8) | raw[9]);
    sample->gyro_y_raw = (int16_t)((raw[10] << 8) | raw[11]);
    sample->gyro_z_raw = (int16_t)((raw[12] << 8) | raw[13]);

    return ESP_OK;
}

/**
 * @brief Habilita o deshabilita el uso del FIFO para acelerometro + giroscopio.
 *
 * @param enable true para habilitar FIFO, false para deshabilitarlo.
 * @return esp_err_t ESP_OK si la operacion fue correcta, o un codigo de error en caso contrario.
 */
esp_err_t mpu6050_enable_fifo(bool enable)
{
    esp_err_t err;

    if (s_mpu_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (enable) {
        err = mpu6050_update_reg_bits(MPU6050_REG_USER_CTRL, 0x40, 0x40);
        if (err != ESP_OK) {
            return err;
        }

        /* FIFO_EN: GYRO XYZ + ACCEL (0x78). */
        return mpu6050_write_reg(MPU6050_REG_FIFO_EN, 0x78);
    }

    err = mpu6050_write_reg(MPU6050_REG_FIFO_EN, 0x00);
    if (err != ESP_OK) {
        return err;
    }

    return mpu6050_update_reg_bits(MPU6050_REG_USER_CTRL, 0x40, 0x00);
}

/**
 * @brief Reinicia el FIFO para descartar muestras antiguas.
 *
 * @return esp_err_t ESP_OK si el reset fue correcto, o un codigo de error en caso contrario.
 */
esp_err_t mpu6050_reset_fifo(void)
{
    esp_err_t err;

    if (s_mpu_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    err = mpu6050_write_reg(MPU6050_REG_FIFO_EN, 0x00);
    if (err != ESP_OK) {
        return err;
    }

    /* USER_CTRL bit2 = FIFO_RESET. */
    return mpu6050_update_reg_bits(MPU6050_REG_USER_CTRL, 0x04, 0x04);
}

/**
 * @brief Obtiene la cantidad de bytes acumulados actualmente en FIFO.
 *
 * @param fifo_count Puntero de salida con el numero de bytes en FIFO.
 * @return esp_err_t ESP_OK si la lectura fue correcta, o un codigo de error en caso contrario.
 */
esp_err_t mpu6050_get_fifo_count(uint16_t *fifo_count)
{
    uint8_t raw_count[2] = {0};
    esp_err_t err;

    if (fifo_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mpu_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    err = mpu6050_read_regs(MPU6050_REG_FIFO_COUNTH, raw_count, sizeof(raw_count));
    if (err != ESP_OK) {
        return err;
    }

    *fifo_count = (uint16_t)((raw_count[0] << 8) | raw_count[1]);
    return ESP_OK;
}

/**
 * @brief Lee de forma masiva una rafaga de muestras crudas desde FIFO.
 *
 * @param samples Buffer de salida para almacenar las muestras leidas.
 * @param sample_count Cantidad de muestras a leer.
 * @return esp_err_t ESP_OK si la lectura fue correcta, o un codigo de error en caso contrario.
 */
esp_err_t mpu6050_read_fifo_burst(mpu6050_sample_t *samples, size_t sample_count)
{
    size_t total_bytes;
    uint8_t *raw_fifo = NULL;
    esp_err_t err;

    if (samples == NULL || sample_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mpu_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    total_bytes = sample_count * MPU6050_FIFO_PACKET_BYTES;
    raw_fifo = (uint8_t *)malloc(total_bytes);
    if (raw_fifo == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = mpu6050_read_regs(MPU6050_REG_FIFO_R_W, raw_fifo, total_bytes);
    if (err == ESP_OK) {
        for (size_t i = 0; i < sample_count; i++) {
            size_t o = i * MPU6050_FIFO_PACKET_BYTES;
            samples[i].accel_x_raw = (int16_t)((raw_fifo[o + 0] << 8) | raw_fifo[o + 1]);
            samples[i].accel_y_raw = (int16_t)((raw_fifo[o + 2] << 8) | raw_fifo[o + 3]);
            samples[i].accel_z_raw = (int16_t)((raw_fifo[o + 4] << 8) | raw_fifo[o + 5]);
            samples[i].gyro_x_raw = (int16_t)((raw_fifo[o + 6] << 8) | raw_fifo[o + 7]);
            samples[i].gyro_y_raw = (int16_t)((raw_fifo[o + 8] << 8) | raw_fifo[o + 9]);
            samples[i].gyro_z_raw = (int16_t)((raw_fifo[o + 10] << 8) | raw_fifo[o + 11]);
        }
    }

    free(raw_fifo);
    return err;
}

/**
 * @brief Configura la deteccion de movimiento (WOM) del MPU6050.
 *
 * @param threshold Umbral de movimiento (registro MOT_THR).
 * @param duration Duracion minima del movimiento (registro MOT_DUR).
 * @return esp_err_t ESP_OK si la configuracion fue correcta, o un codigo de error en caso contrario.
 */
esp_err_t mpu6050_setup_motion_interrupt(uint8_t threshold, uint8_t duration)
{
    esp_err_t err;
    uint8_t accel_cfg = 0;

    if (s_mpu_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    err = mpu6050_read_regs(MPU6050_REG_ACCEL_CONFIG, &accel_cfg, 1);
    if (err != ESP_OK) {
        return err;
    }

    /* DHPF=1 (5Hz) para que WOM responda a variaciones de movimiento. */
    accel_cfg = (uint8_t)((accel_cfg & 0xF8) | 0x01);
    err = mpu6050_write_reg(MPU6050_REG_ACCEL_CONFIG, accel_cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = mpu6050_write_reg(MPU6050_REG_MOT_THR, threshold);
    if (err != ESP_OK) {
        return err;
    }

    err = mpu6050_write_reg(MPU6050_REG_MOT_DUR, duration);
    if (err != ESP_OK) {
        return err;
    }

    /* INT_PIN_CFG: latch INT until read, active high, push-pull. */
    err = mpu6050_write_reg(MPU6050_REG_INT_PIN_CFG, 0x30);
    if (err != ESP_OK) {
        return err;
    }

    /* Bit6 de INT_ENABLE activa la interrupcion de movimiento. */
    err = mpu6050_write_reg(MPU6050_REG_INT_ENABLE, 0x40);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG,
             "WOM configurado: MOT_THR=%u MOT_DUR=%u ACCEL_CONFIG=0x%02X INT_PIN_CFG=0x30",
             threshold,
             duration,
             accel_cfg);

    return ESP_OK;
}

/**
 * @brief Limpia la interrupcion pendiente leyendo el registro INT_STATUS.
 *
 * @return esp_err_t ESP_OK si la lectura fue correcta, o un codigo de error en caso contrario.
 */
esp_err_t mpu6050_clear_interrupt_status(void)
{
    uint8_t int_status = 0;
    esp_err_t err;

    if (s_mpu_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    err = mpu6050_read_regs(MPU6050_REG_INT_STATUS, &int_status, 1);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "INT_STATUS leido para limpiar interrupcion pendiente: 0x%02X", int_status);
    }

    return err;
}
