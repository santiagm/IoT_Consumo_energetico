#ifndef MPU6050_H
#define MPU6050_H

#include <stdint.h>
#include "esp_err.h"
#include "telemetry_packet.h"

esp_err_t mpu6050_init(void);
esp_err_t mpu6050_wake(void);
esp_err_t mpu6050_sleep(void);

esp_err_t mpu6050_read_sample(mpu_sample_t *sample);
esp_err_t mpu6050_read_samples(mpu_sample_t *samples, uint8_t count);

esp_err_t mpu6050_clear_interrupt(void);
esp_err_t mpu6050_config_wom(void);

#endif