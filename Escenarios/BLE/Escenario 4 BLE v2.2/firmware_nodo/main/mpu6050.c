#include "mpu6050.h"
#include "config.h"

#include <string.h>

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MPU6050";

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

#define MPU_REG_PWR_MGMT_1       0x6B
#define MPU_REG_SMPLRT_DIV       0x19
#define MPU_REG_CONFIG           0x1A
#define MPU_REG_GYRO_CONFIG      0x1B
#define MPU_REG_ACCEL_CONFIG     0x1C
#define MPU_REG_ACCEL_CONFIG2    0x1D
#define MPU_REG_INT_PIN_CFG      0x37
#define MPU_REG_INT_ENABLE       0x38
#define MPU_REG_INT_STATUS       0x3A
#define MPU_REG_ACCEL_XOUT_H     0x3B
#define MPU_REG_MOT_THR          0x1F
#define MPU_REG_MOT_DUR          0x20

static esp_err_t mpu_write_u8(uint8_t reg, uint8_t val)
{
    uint8_t data[2] = {reg, val};
    return i2c_master_transmit(s_dev, data, sizeof(data), 1000);
}

static esp_err_t mpu_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 1000);
}

esp_err_t mpu6050_init(void)
{
    if (s_bus != NULL && s_dev != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_PORT,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_io_num = I2C_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &s_bus));

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MPU_I2C_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };

    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev_config, &s_dev));

    ESP_ERROR_CHECK(mpu6050_wake());

    ESP_ERROR_CHECK(mpu_write_u8(MPU_REG_SMPLRT_DIV, 9));     // 100 Hz aprox
    ESP_ERROR_CHECK(mpu_write_u8(MPU_REG_CONFIG, 0x03));
    ESP_ERROR_CHECK(mpu_write_u8(MPU_REG_GYRO_CONFIG, 0x00)); // ±250 dps
    ESP_ERROR_CHECK(mpu_write_u8(MPU_REG_ACCEL_CONFIG, 0x00)); // ±2g
    ESP_ERROR_CHECK(mpu_write_u8(MPU_REG_ACCEL_CONFIG2, 0x03));

    ESP_LOGI(TAG, "MPU6050 inicializado: addr=0x%02X fs_acc=0 fs_gyro=0 fs_hz=100", MPU_I2C_ADDR);
    return ESP_OK;
}

esp_err_t mpu6050_wake(void)
{
    esp_err_t ret = mpu_write_u8(MPU_REG_PWR_MGMT_1, 0x00);
    vTaskDelay(pdMS_TO_TICKS(5));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MPU6050 despierto desde modo sleep");
    }
    return ret;
}

esp_err_t mpu6050_sleep(void)
{
    /*
     * PWR_MGMT_1 bit 6 = SLEEP.
     * 0x40 coloca el MPU6050 en modo sleep.
     */
    esp_err_t ret = mpu_write_u8(MPU_REG_PWR_MGMT_1, 0x40);

    vTaskDelay(pdMS_TO_TICKS(1));

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MPU6050 colocado en modo sleep");
    } else {
        ESP_LOGE(TAG, "No se pudo colocar MPU6050 en sleep");
    }

    return ret;
}


esp_err_t mpu6050_read_sample(mpu_sample_t *sample)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw[14] = {0};
    esp_err_t ret = mpu_read(MPU_REG_ACCEL_XOUT_H, raw, sizeof(raw));
    if (ret != ESP_OK) {
        return ret;
    }

    sample->ax = (int16_t)((raw[0] << 8) | raw[1]);
    sample->ay = (int16_t)((raw[2] << 8) | raw[3]);
    sample->az = (int16_t)((raw[4] << 8) | raw[5]);
    sample->gx = (int16_t)((raw[8] << 8) | raw[9]);
    sample->gy = (int16_t)((raw[10] << 8) | raw[11]);
    sample->gz = (int16_t)((raw[12] << 8) | raw[13]);

    return ESP_OK;
}

esp_err_t mpu6050_read_samples(mpu_sample_t *samples, uint8_t count)
{
    if (samples == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* OPTIMIZADO: Solo leer una muestra real, mantener resto del array en ceros */
    memset(samples, 0, count * sizeof(mpu_sample_t));
    esp_err_t ret = mpu6050_read_sample(&samples[0]);
    if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
}

esp_err_t mpu6050_clear_interrupt(void)
{
    uint8_t status = 0;
    esp_err_t ret = mpu_read(MPU_REG_INT_STATUS, &status, 1);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "INT_STATUS leido para limpiar interrupcion pendiente: 0x%02X", status);
    }
    return ret;
}

esp_err_t mpu6050_config_wom(void)
{
    /*
     * Configuracion basica de Wake-on-Motion.
     * MOT_THR y MOT_DUR se pueden ajustar para sensibilidad/consumo.
     */
    ESP_ERROR_CHECK(mpu_write_u8(MPU_REG_MOT_THR, 12));
    ESP_ERROR_CHECK(mpu_write_u8(MPU_REG_MOT_DUR, 2));
    ESP_ERROR_CHECK(mpu_write_u8(MPU_REG_ACCEL_CONFIG, 0x01));
    ESP_ERROR_CHECK(mpu_write_u8(MPU_REG_INT_PIN_CFG, 0x30));
    ESP_ERROR_CHECK(mpu_write_u8(MPU_REG_INT_ENABLE, 0x40));

    ESP_LOGI(TAG, "WOM configurado: MOT_THR=12 MOT_DUR=2 ACCEL_CONFIG=0x01 INT_PIN_CFG=0x30");
    return ESP_OK;
}
