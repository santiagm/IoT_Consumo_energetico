#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_now.h"
#include "esp_wifi.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"

#include "config.h"
#include "espnow_handler.h"
#include "mpu6050.h"
#include "security_payload.h"

#include "psa/crypto.h"

/*
 * ============================================================
 * ESCENARIO 4
 * ============================================================
 *
 * ESP-NOW + Seguridad PMK/LMK + AES-128-GCM de aplicacion + MPU6050 + Deep Sleep con RTC = 10 segundos.
 *
 * No usa:
 * - MQTT en el nodo
 * - GPIO INT del MPU como wakeup
 *
 * Sí usa seguridad ESP-NOW:
 * - PMK
 * - LMK por peer
 * - peer_info.encrypt = true
 *
 * Seguridad adicional de aplicacion:
 * - AES-128-GCM sobre sensor_packet_t completo
 *
 * Sí usa:
 * - MPU6050
 * - ESP-NOW con cifrado PMK/LMK
 * - WiFi STA solo para habilitar ESP-NOW
 * - Deep sleep por timer RTC cada 10 segundos
 *
 * Objetivo:
 * Medir consumo del nodo con ESP-NOW + MPU6050,
 * dejando ESP32-C6 y MPU6050 en sleep/deep sleep.
 *
 * Medición:
 * - Jumper J5 quitado.
 * - Fuente V+ -> J5-A.
 * - J5-A -> I+ Joulescope.
 * - I- Joulescope -> J5-B.
 * - VCC MPU6050 -> J5-B / I- Joulescope.
 * - Fuente V- -> GND común.
 * - GND MPU6050 -> GND común.
 * - V+ Joulescope -> J5-B.
 * - V- Joulescope -> GND común.
 *
 * Conexión MPU6050:
 * - VCC -> J5-B / I-
 * - GND -> GND
 * - SDA -> GPIO6
 * - SCL -> GPIO7
 * - INT -> GPIO4
 * - ADO -> VCC, dirección I2C 0x69
 */

/*
 * NODE_DEBUG_LOGS = 1:
 *   Modo debug. Imprime por monitor serial.
 *
 * NODE_DEBUG_LOGS = 0:
 *   Modo medición energética. Reduce logs UART.
 */
#define NODE_DEBUG_LOGS 0

#if NODE_DEBUG_LOGS
static const char *TAG = "NODO_SENSOR_ESC4_SEC";
#define NODE_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define NODE_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#else
static const char *TAG = "NODO_SENSOR_ESC4_SEC";
#define NODE_LOGI(tag, fmt, ...)
#define NODE_LOGW(tag, fmt, ...)
#endif

/*
 * RTC = 10 segundos.
 */
#define RTC_WAKEUP_TIME_US   (5ULL * 1000000ULL)

/*
 * Tiempo máximo de espera para que el FIFO tenga suficientes muestras.
 * No es el tiempo total de grabación, solo un límite de seguridad.
 */
#define MPU_FIFO_TIMEOUT_MS  30U

/*
 * ESP32-C6-DevKitC-1:
 * LED RGB integrado controlado por GPIO8.
 * El LED rojo de power de la placa NO se puede apagar por firmware.
 */
#define RGB_LED_GPIO GPIO_NUM_8

#ifndef I2C_SDA_GPIO
#define I2C_SDA_GPIO GPIO_NUM_6
#endif

#ifndef I2C_SCL_GPIO
#define I2C_SCL_GPIO GPIO_NUM_7
#endif

#ifndef MPU_INT_GPIO
#define MPU_INT_GPIO GPIO_NUM_4
#endif

/*
 * Batch ID persistente entre ciclos de deep sleep.
 */
RTC_DATA_ATTR static uint16_t batch_id = 0;

typedef struct __attribute__((packed)) {
    int16_t ax;
    int16_t ay;
    int16_t az;
    int16_t gx;
    int16_t gy;
    int16_t gz;
} mpu_sample_t;

typedef struct __attribute__((packed)) {
    char src_mac[18];
    uint32_t start_timestamp_ms;
    uint16_t batch_id;
    mpu_sample_t muestras[STREAM_SAMPLE_COUNT];
} sensor_packet_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint16_t batch_id;
    uint16_t encrypted_len;
    uint8_t iv[AES_GCM_IV_LEN];
} secure_packet_aad_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint16_t batch_id;
    uint16_t encrypted_len;
    uint8_t iv[AES_GCM_IV_LEN];
    uint8_t encrypted_payload[sizeof(sensor_packet_t)];
    uint8_t auth_tag[AES_GCM_TAG_LEN];
} secure_espnow_packet_t;

_Static_assert(sizeof(sensor_packet_t) <= ESP_NOW_MAX_DATA_LEN,
               "sensor_packet_t exceeds ESP-NOW payload limit. Reduce STREAM_SAMPLE_COUNT");

_Static_assert(sizeof(secure_espnow_packet_t) <= ESP_NOW_MAX_DATA_LEN,
               "secure_espnow_packet_t exceeds ESP-NOW payload limit. Reduce STREAM_SAMPLE_COUNT");

static const uint8_t s_app_aes_key[16] = APP_AES_GCM_KEY_BYTES;

static uint32_t get_timestamp_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void mac_to_string(const uint8_t *mac, char *mac_str, size_t mac_str_len)
{
    snprintf(mac_str, mac_str_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void build_gcm_iv(uint8_t iv[AES_GCM_IV_LEN],
                         const uint8_t mac[6],
                         uint16_t current_batch_id,
                         uint32_t timestamp_ms)
{
    /*
     * IV de 12 bytes para AES-GCM.
     * Formato: prefijo ESP-NOW app-sec + 4 bytes de MAC + batch_id + timestamp_ms.
     * Para una misma clave AES evita reutilizar IV mientras batch_id/timestamp avancen.
     */
    iv[0] = 'E';
    iv[1] = 'N';
    memcpy(&iv[2], &mac[2], 4);
    iv[6] = (uint8_t)(current_batch_id & 0xFFU);
    iv[7] = (uint8_t)((current_batch_id >> 8) & 0xFFU);
    iv[8] = (uint8_t)(timestamp_ms & 0xFFU);
    iv[9] = (uint8_t)((timestamp_ms >> 8) & 0xFFU);
    iv[10] = (uint8_t)((timestamp_ms >> 16) & 0xFFU);
    iv[11] = (uint8_t)((timestamp_ms >> 24) & 0xFFU);
}

static void fill_secure_aad(const secure_espnow_packet_t *secure_packet,
                            secure_packet_aad_t *aad)
{
    aad->magic = secure_packet->magic;
    aad->version = secure_packet->version;
    aad->batch_id = secure_packet->batch_id;
    aad->encrypted_len = secure_packet->encrypted_len;
    memcpy(aad->iv, secure_packet->iv, sizeof(aad->iv));
}

static esp_err_t encrypt_sensor_packet_gcm(const sensor_packet_t *plain_packet,
                                           secure_espnow_packet_t *secure_packet)
{
    if (plain_packet == NULL || secure_packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t sta_mac[6] = {0};
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, sta_mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo leer MAC STA para IV AES-GCM: %s", esp_err_to_name(err));
        return err;
    }

    memset(secure_packet, 0, sizeof(*secure_packet));
    secure_packet->magic = ESPNOW_APP_SEC_MAGIC;
    secure_packet->version = ESPNOW_APP_SEC_VERSION;
    secure_packet->batch_id = plain_packet->batch_id;
    secure_packet->encrypted_len = (uint16_t)sizeof(sensor_packet_t);
    build_gcm_iv(secure_packet->iv, sta_mac, plain_packet->batch_id, plain_packet->start_timestamp_ms);

    secure_packet_aad_t aad;
    fill_secure_aad(secure_packet, &aad);

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init fallo: %ld", (long)status);
        return ESP_FAIL;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;

    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);

    status = psa_import_key(&attributes,
                            s_app_aes_key,
                            sizeof(s_app_aes_key),
                            &key_id);
    psa_reset_key_attributes(&attributes);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key AES-GCM fallo: %ld", (long)status);
        return ESP_FAIL;
    }

    uint8_t aead_output[sizeof(sensor_packet_t) + AES_GCM_TAG_LEN] = {0};
    size_t aead_output_len = 0;

    status = psa_aead_encrypt(key_id,
                              PSA_ALG_GCM,
                              secure_packet->iv,
                              AES_GCM_IV_LEN,
                              (const uint8_t *)&aad,
                              sizeof(aad),
                              (const uint8_t *)plain_packet,
                              sizeof(sensor_packet_t),
                              aead_output,
                              sizeof(aead_output),
                              &aead_output_len);

    (void)psa_destroy_key(key_id);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "AES-128-GCM cifrado fallo PSA: %ld", (long)status);
        return ESP_FAIL;
    }

    if (aead_output_len != (sizeof(sensor_packet_t) + AES_GCM_TAG_LEN)) {
        ESP_LOGE(TAG,
                 "AES-GCM longitud invalida: %u esperado=%u",
                 (unsigned)aead_output_len,
                 (unsigned)(sizeof(sensor_packet_t) + AES_GCM_TAG_LEN));
        return ESP_FAIL;
    }

    memcpy(secure_packet->encrypted_payload, aead_output, sizeof(sensor_packet_t));
    memcpy(secure_packet->auth_tag, &aead_output[sizeof(sensor_packet_t)], AES_GCM_TAG_LEN);

    NODE_LOGI(TAG,
              "AES-128-GCM OK batch=%u plain_len=%u secure_len=%u",
              (unsigned)plain_packet->batch_id,
              (unsigned)sizeof(sensor_packet_t),
              (unsigned)sizeof(secure_espnow_packet_t));

    return ESP_OK;
}

static void set_gpio_input_floating(gpio_num_t gpio)
{
    if (gpio < GPIO_NUM_0 || gpio >= GPIO_NUM_MAX) {
        return;
    }

    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(gpio, GPIO_FLOATING);
}

/*
 * Prepara GPIOs antes de deep sleep.
 *
 * Importante:
 * - No se tocan GPIO16/GPIO17 porque son UART consola.
 * - No se tocan GPIO12/GPIO13 por posible USB nativo.
 * - GPIO4/GPIO6/GPIO7 se liberan después de dormir el MPU6050.
 * - GPIO8 se libera para evitar consumo del LED RGB integrado.
 */
static void prepare_gpios_for_deep_sleep(void)
{
    /*
     * Pines usados por el MPU6050.
     * Se liberan solo antes de dormir, después de terminar I2C.
     */
    set_gpio_input_floating((gpio_num_t)I2C_SDA_GPIO);   // GPIO6
    set_gpio_input_floating((gpio_num_t)I2C_SCL_GPIO);   // GPIO7
    set_gpio_input_floating((gpio_num_t)MPU_INT_GPIO);   // GPIO4

    /*
     * LED RGB integrado.
     */
    set_gpio_input_floating(RGB_LED_GPIO);               // GPIO8

    /*
     * Otros GPIOs no usados.
     *
     * Excluidos:
     * - GPIO4/GPIO6/GPIO7: ya configurados arriba.
     * - GPIO8: LED RGB, ya configurado arriba.
     * - GPIO12/GPIO13: USB nativo.
     * - GPIO16/GPIO17: UART consola.
     */
    const gpio_num_t unused_gpios[] = {
        GPIO_NUM_0,
        GPIO_NUM_1,
        GPIO_NUM_2,
        GPIO_NUM_3,
        GPIO_NUM_5,
        GPIO_NUM_9,
        GPIO_NUM_10,
        GPIO_NUM_11,
        GPIO_NUM_14,
        GPIO_NUM_15,
        GPIO_NUM_18,
        GPIO_NUM_19,
        GPIO_NUM_20,
        GPIO_NUM_21,
        GPIO_NUM_22,
        GPIO_NUM_23,
    };

    for (size_t i = 0; i < sizeof(unused_gpios) / sizeof(unused_gpios[0]); i++) {
        set_gpio_input_floating(unused_gpios[i]);
    }

    NODE_LOGI(TAG, "GPIOs preparados para deep sleep");
}

static void log_wakeup_cause(void)
{
#if NODE_DEBUG_LOGS
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    switch (cause) {
        case ESP_SLEEP_WAKEUP_TIMER:
            ESP_LOGI(TAG, "Wakeup: TIMER RTC");
            break;

        case ESP_SLEEP_WAKEUP_UNDEFINED:
            ESP_LOGI(TAG, "Wakeup: arranque inicial / cold boot");
            break;

        default:
            ESP_LOGI(TAG, "Wakeup cause: %d", (int)cause);
            break;
    }
#endif
}

static esp_err_t read_mpu_fifo_packet(sensor_packet_t *packet)
{
    if (packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;
    uint16_t fifo_bytes = 0;
    const uint16_t bytes_needed = (uint16_t)(STREAM_SAMPLE_COUNT * sizeof(mpu6050_sample_t));
    mpu6050_sample_t raw_samples[STREAM_SAMPLE_COUNT];

    memset(packet, 0, sizeof(*packet));

    uint8_t sta_mac[6] = {0};
    err = esp_wifi_get_mac(WIFI_IF_STA, sta_mac);
    if (err == ESP_OK) {
        mac_to_string(sta_mac, packet->src_mac, sizeof(packet->src_mac));
    } else {
        strncpy(packet->src_mac, "00:00:00:00:00:00", sizeof(packet->src_mac));
        packet->src_mac[sizeof(packet->src_mac) - 1] = '\0';
        NODE_LOGW(TAG, "No se pudo leer MAC STA para src_mac: %s", esp_err_to_name(err));
    }

    /*
     * Reiniciamos FIFO para capturar un bloque limpio de muestras.
     */
    err = mpu6050_enable_fifo(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo deshabilitar FIFO: %s", esp_err_to_name(err));
        return err;
    }

    err = mpu6050_reset_fifo();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo resetear FIFO: %s", esp_err_to_name(err));
        return err;
    }

    err = mpu6050_enable_fifo(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo habilitar FIFO: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Esperamos hasta tener suficientes muestras para un solo paquete.
     */
    int64_t start_us = esp_timer_get_time();
    const int64_t timeout_us = (int64_t)MPU_FIFO_TIMEOUT_MS * 1000LL;

    do {
        err = mpu6050_get_fifo_count(&fifo_bytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo leer FIFO count: %s", esp_err_to_name(err));
            return err;
        }

        if (fifo_bytes >= bytes_needed) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    } while ((esp_timer_get_time() - start_us) < timeout_us);

    if (fifo_bytes < bytes_needed) {
        ESP_LOGE(TAG,
                 "FIFO insuficiente: bytes=%u esperado=%u",
                 (unsigned)fifo_bytes,
                 (unsigned)bytes_needed);
        return ESP_ERR_TIMEOUT;
    }

    err = mpu6050_read_fifo_burst(raw_samples, STREAM_SAMPLE_COUNT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo leer FIFO burst: %s", esp_err_to_name(err));
        return err;
    }

    packet->start_timestamp_ms = get_timestamp_ms();
    packet->batch_id = batch_id++;

    for (uint8_t i = 0; i < STREAM_SAMPLE_COUNT; i++) {
        packet->muestras[i].ax = raw_samples[i].accel_x_raw;
        packet->muestras[i].ay = raw_samples[i].accel_y_raw;
        packet->muestras[i].az = raw_samples[i].accel_z_raw;
        packet->muestras[i].gx = raw_samples[i].gyro_x_raw;
        packet->muestras[i].gy = raw_samples[i].gyro_y_raw;
        packet->muestras[i].gz = raw_samples[i].gyro_z_raw;
    }

    NODE_LOGI(TAG,
              "Paquete MPU listo batch=%u timestamp=%" PRIu32 " len=%u",
              (unsigned)packet->batch_id,
              packet->start_timestamp_ms,
              (unsigned)sizeof(*packet));

    return ESP_OK;
}

static esp_err_t send_packet(const sensor_packet_t *packet)
{
    if (packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    secure_espnow_packet_t secure_packet;
    esp_err_t err = encrypt_sensor_packet_gcm(packet, &secure_packet);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fallo cifrado AES-128-GCM: %s", esp_err_to_name(err));
        return err;
    }

    err = espnow_handler_send((const uint8_t *)&secure_packet, sizeof(secure_packet));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fallo envio ESP-NOW: %s", esp_err_to_name(err));
        return err;
    }

    NODE_LOGI(TAG,
              "Paquete ESP-NOW seguro enviado correctamente batch=%u len=%u",
              (unsigned)packet->batch_id,
              (unsigned)sizeof(secure_packet));
    return ESP_OK;
}

static void enter_deep_sleep(void)
{
    esp_err_t err;

    /*
     * Apagar FIFO y dormir MPU6050 antes de deep sleep.
     */
    err = mpu6050_enable_fifo(false);
    if (err != ESP_OK) {
        NODE_LOGW(TAG, "No se pudo deshabilitar FIFO antes de dormir: %s", esp_err_to_name(err));
    }

    err = mpu6050_reset_fifo();
    if (err != ESP_OK) {
        NODE_LOGW(TAG, "No se pudo resetear FIFO antes de dormir: %s", esp_err_to_name(err));
    }

    err = mpu6050_sleep();
    if (err != ESP_OK) {
        NODE_LOGW(TAG, "No se pudo poner MPU6050 en sleep: %s", esp_err_to_name(err));
    }

    /*
     * Liberar el driver I2C antes de poner SDA/SCL en alta impedancia.
     * Esto evita que el periférico I2C mantenga las líneas activas durante deep sleep.
     */
    err = mpu6050_deinit_bus();
    if (err != ESP_OK) {
        NODE_LOGW(TAG, "No se pudo liberar bus I2C antes de dormir: %s", esp_err_to_name(err));
    }

    /*
     * Cerrar ESP-NOW y WiFi antes de deep sleep.
     */
    NODE_LOGI(TAG, "Cerrando ESP-NOW antes de deep sleep");
    esp_now_deinit();

    NODE_LOGI(TAG, "Deteniendo WiFi antes de deep sleep");
    esp_wifi_stop();

    /*
     * Liberar GPIOs después de terminar I2C y WiFi.
     */
    prepare_gpios_for_deep_sleep();

    /*
     * Wakeup por temporizador RTC cada 10 segundos.
     */
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(RTC_WAKEUP_TIME_US));

#if NODE_DEBUG_LOGS
    ESP_LOGI(TAG, "Entrando a deep sleep por 10 segundos");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
#endif

    esp_deep_sleep_start();
}

void app_main(void)
{
#if NODE_DEBUG_LOGS
    esp_log_level_set(TAG, ESP_LOG_INFO);
#else
    /*
     * En medición energética se reducen logs por UART.
     * Se dejan errores activos para diagnóstico crítico.
     */
    esp_log_level_set("*", ESP_LOG_ERROR);
#endif

    log_wakeup_cause();

    NODE_LOGI(TAG, "ESCENARIO 4: ESP-NOW + Seguridad + MPU6050 + deep sleep RTC = 10 s");

    /*
     * Devolvemos control digital a los pines usados por I2C/INT
     * después de un ciclo de deep sleep.
     */
    rtc_gpio_deinit((gpio_num_t)I2C_SDA_GPIO);
    rtc_gpio_deinit((gpio_num_t)I2C_SCL_GPIO);
    rtc_gpio_deinit((gpio_num_t)MPU_INT_GPIO);

    esp_err_t err;

    mpu6050_config_t mpu_cfg = {
        .sda_io = I2C_SDA_GPIO,
        .scl_io = I2C_SCL_GPIO,
        .i2c_addr = MPU_I2C_ADDR,
        .accel_full_scale = MPU6050_ACCEL_FS_2G,
        .gyro_full_scale = MPU6050_GYRO_FS_250DPS,
        .sample_rate_hz = MPU_SAMPLE_RATE_HZ,
    };

    /*
     * Inicializar MPU6050.
     */
    err = mpu6050_init(&mpu_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo inicializar MPU6050: %s", esp_err_to_name(err));
        prepare_gpios_for_deep_sleep();
        ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(RTC_WAKEUP_TIME_US));
        esp_deep_sleep_start();
    }

    /*
     * Inicializar ESP-NOW con seguridad.
     * El handler agrega el gateway como peer cifrado con PMK/LMK.
     */
    err = espnow_handler_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo inicializar ESP-NOW: %s", esp_err_to_name(err));
        enter_deep_sleep();
    }

    /*
     * Potencia TX.
     * Puedes probar valores menores:
     * 20 = 5 dBm
     * 8  = 2 dBm
     * 4  = 1 dBm
     *
     * Para referencia dejamos 5 dBm.
     */
    err = esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo configurar potencia TX WiFi: %s", esp_err_to_name(err));
        enter_deep_sleep();
    }

    int8_t tx_power_qdbm = 0;
    esp_wifi_get_max_tx_power(&tx_power_qdbm);
    ESP_LOGI(TAG, "Potencia TX ESP-NOW configurada: raw=%d, %.2f dBm",
            tx_power_qdbm, tx_power_qdbm / 4.0f);

    /*
     * Leer un bloque de muestras y enviar un solo paquete ESP-NOW.
     */
    sensor_packet_t packet;
    err = read_mpu_fifo_packet(&packet);
    if (err == ESP_OK) {
        (void)send_packet(&packet);
    } else {
        ESP_LOGE(TAG, "No se pudo preparar paquete MPU6050: %s", esp_err_to_name(err));
    }

    /*
     * Espera corta para completar TX antes de apagar radio.
     */
    vTaskDelay(pdMS_TO_TICKS(5));

    enter_deep_sleep();
}