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
#include "esp_system.h"
#include "esp_random.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"

#include "config.h"
#include "espnow_handler.h"


#define NODE_DEBUG_LOGS 0

#if NODE_DEBUG_LOGS
static const char *TAG = "NODO_SENSOR_ESC2";
#define NODE_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define NODE_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#else
static const char *TAG = "NODO_SENSOR_ESC2";
#define NODE_LOGI(tag, fmt, ...)
#define NODE_LOGW(tag, fmt, ...)
#endif

/*
 * RTC = 5 segundos.
 * Se conserva el valor exacto heredado del Escenario 4.
 */
#define RTC_WAKEUP_TIME_US   (5ULL * 1000000ULL)

/*
 * ESP32-C6-DevKitC-1:
 * LED RGB integrado controlado por GPIO8.
 * El LED rojo de power de la placa NO se puede apagar por firmware.
 */
#define RGB_LED_GPIO GPIO_NUM_8

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

_Static_assert(sizeof(sensor_packet_t) <= ESP_NOW_MAX_DATA_LEN,
               "sensor_packet_t exceeds ESP-NOW payload limit. Reduce STREAM_SAMPLE_COUNT");

static uint32_t get_timestamp_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void mac_to_string(const uint8_t *mac, char *mac_str, size_t mac_str_len)
{
    snprintf(mac_str, mac_str_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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
 * - GPIO8 se libera para evitar consumo del LED RGB integrado.
 */
static void prepare_gpios_for_deep_sleep(void)
{
    /*
     * LED RGB integrado.
     */
    set_gpio_input_floating(RGB_LED_GPIO);               // GPIO8

    /*
     * Otros GPIOs no usados.
     *
     * Excluidos:
     * - GPIO8: LED RGB, ya configurado arriba.
     * - GPIO12/GPIO13: USB nativo.
     * - GPIO16/GPIO17: UART consola.
     */
    const gpio_num_t unused_gpios[] = {
        GPIO_NUM_0,
        GPIO_NUM_1,
        GPIO_NUM_2,
        GPIO_NUM_3,
        GPIO_NUM_4,
        GPIO_NUM_5,
        GPIO_NUM_6,
        GPIO_NUM_7,
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

static int16_t synthetic_value(uint16_t current_batch_id,
                               uint8_t sample_index,
                               uint8_t axis_index,
                               int16_t center,
                               int16_t amplitude)
{
    uint32_t random_part = esp_random();
    int32_t span = (int32_t)(amplitude * 2) + 1;
    int32_t offset = (int32_t)(random_part % (uint32_t)span) - amplitude;
    int32_t trend = ((int32_t)current_batch_id * (axis_index + 3) +
                     (int32_t)sample_index * (axis_index + 5)) % 17;

    return (int16_t)(center + offset + trend);
}

static esp_err_t build_synthetic_packet(sensor_packet_t *packet)
{
    if (packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(packet, 0, sizeof(*packet));

    uint8_t sta_mac[6] = {0};
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, sta_mac);
    if (err == ESP_OK) {
        mac_to_string(sta_mac, packet->src_mac, sizeof(packet->src_mac));
    } else {
        strncpy(packet->src_mac, "00:00:00:00:00:00", sizeof(packet->src_mac));
        packet->src_mac[sizeof(packet->src_mac) - 1] = '\0';
        NODE_LOGW(TAG, "No se pudo leer MAC STA para src_mac: %s", esp_err_to_name(err));
    }

    packet->start_timestamp_ms = get_timestamp_ms();
    packet->batch_id = batch_id++;

    for (uint8_t i = 0; i < STREAM_SAMPLE_COUNT; i++) {
        packet->muestras[i].ax = synthetic_value(packet->batch_id, i, 0, 0, 420);
        packet->muestras[i].ay = synthetic_value(packet->batch_id, i, 1, 0, 420);
        packet->muestras[i].az = synthetic_value(packet->batch_id, i, 2, 16384, 650);
        packet->muestras[i].gx = synthetic_value(packet->batch_id, i, 3, 0, 180);
        packet->muestras[i].gy = synthetic_value(packet->batch_id, i, 4, 0, 180);
        packet->muestras[i].gz = synthetic_value(packet->batch_id, i, 5, 0, 180);
    }

    NODE_LOGI(TAG,
              "Paquete sintetico listo batch=%u timestamp=%" PRIu32 " len=%u",
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

    esp_err_t err = espnow_handler_send((const uint8_t *)packet, sizeof(*packet));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fallo envio ESP-NOW plano: %s", esp_err_to_name(err));
        return err;
    }

    NODE_LOGI(TAG,
              "Paquete ESP-NOW plano enviado correctamente batch=%u len=%u",
              (unsigned)packet->batch_id,
              (unsigned)sizeof(*packet));
    return ESP_OK;
}

static void enter_deep_sleep(void)
{
    /*
     * Cerrar ESP-NOW y WiFi antes de deep sleep.
     */
    NODE_LOGI(TAG, "Cerrando ESP-NOW antes de deep sleep");
    esp_now_deinit();

    NODE_LOGI(TAG, "Deteniendo WiFi antes de deep sleep");
    esp_wifi_stop();

    /*
     * Liberar GPIOs después de terminar WiFi.
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

    NODE_LOGI(TAG, "ESCENARIO 2: ESP-NOW puro + datos sinteticos + deep sleep RTC = 10 s");

    /*
     * Se devuelven a control digital los GPIO que antes se usaban para MPU/I2C.
     * No inicializa I2C ni MPU6050 en este escenario.
     */
    rtc_gpio_deinit(GPIO_NUM_4);
    rtc_gpio_deinit(GPIO_NUM_6);
    rtc_gpio_deinit(GPIO_NUM_7);

    esp_err_t err;

    /*
     * Inicializar ESP-NOW sin seguridad nativa.
     * El handler agrega el gateway como peer sin PMK/LMK y encrypt=false.
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
     * Construir un paquete sintetico y enviar un solo paquete ESP-NOW plano.
     */
    sensor_packet_t packet;
    err = build_synthetic_packet(&packet);
    if (err == ESP_OK) {
        (void)send_packet(&packet);
    } else {
        ESP_LOGE(TAG, "No se pudo preparar paquete sintetico: %s", esp_err_to_name(err));
    }

    /*
     * Espera corta para completar TX antes de apagar radio.
     */
    vTaskDelay(pdMS_TO_TICKS(5));

    enter_deep_sleep();
}
