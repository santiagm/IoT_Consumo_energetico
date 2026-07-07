#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_mac.h"

#include "driver/gpio.h"

#include "config.h"
#include "telemetry_packet.h"
#include "thread_packet.h"
#include "crypto_helper.h"
#include "thread_sender.h"

static const char *TAG = "nodo_thread";

RTC_DATA_ATTR static uint16_t rtc_batch_id = 0;

static void fill_src_id(char out[18])
{
#ifdef NODE_ID
    snprintf(out, 18, "%s", NODE_ID);
#else
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#endif
}

static void release_gpios(void)
{
    /*
     * IMPORTANTE:
     * En ESP32-C6 no todos los GPIO son RTC GPIO.
     * Por eso NO usamos rtc_gpio_deinit() aquí.
     * Eso era lo que producía:
     * RTCIO: rtc_gpio_deinit(65): RTCIO number error
     */
    for (size_t i = 0; i < NODE_GPIOS_TO_RELEASE_COUNT; ++i) {
        gpio_num_t gpio = NODE_GPIOS_TO_RELEASE[i];

        gpio_reset_pin(gpio);
        gpio_set_direction(gpio, GPIO_MODE_DISABLE);
        gpio_pullup_dis(gpio);
        gpio_pulldown_dis(gpio);
    }
}

static void deep_sleep_now(void)
{
#if NODE_ENABLE_RTC_PERIODIC_WAKEUP
    ESP_ERROR_CHECK(
        esp_sleep_enable_timer_wakeup(
            (uint64_t)NODE_WAKEUP_PERIOD_SEC * 1000000ULL
        )
    );
#endif

    release_gpios();

    ESP_LOGI(TAG, "Entrando en deep sleep");

    esp_deep_sleep_start();
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    return ret;
}

static void fill_synthetic_batch(sensor_packet_t *pkt)
{
    if (!pkt) {
        return;
    }

    for (int i = 0; i < STREAM_SAMPLE_COUNT; ++i) {
        int16_t base = (int16_t)(pkt->batch_id * 10U + (uint16_t)i);

        pkt->muestras[i].ax = (int16_t)(1000 + base);
        pkt->muestras[i].ay = (int16_t)(-500 + base);
        pkt->muestras[i].az = 16384;
        pkt->muestras[i].gx = (int16_t)(20 + base);
        pkt->muestras[i].gy = (int16_t)(-15 + base);
        pkt->muestras[i].gz = (int16_t)(5 + base);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando N3 Router Thread. Sin deep sleep para mantener la malla.");

    esp_err_t ret = init_nvs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init fallo: %s", esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    /*
     * N3 es Router de malla: no debe entrar en deep sleep.
     * Se liberan GPIOs no usados una sola vez para reducir consumo periférico.
     * No se inicializa MPU, I2C, Wi-Fi, BLE ni MQTT.
     */
    release_gpios();

    int64_t t_stage = esp_timer_get_time();
    ret = thread_sender_init();
    int64_t t_thread_init_ms = (esp_timer_get_time() - t_stage) / 1000;

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "thread_sender_init fallo: %s tiempo_thread_init_ms=%lld",
                 esp_err_to_name(ret), t_thread_init_ms);
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    ESP_LOGI(TAG, "N3 Router Thread activo. Enviara dato propio cada %u ms", (unsigned)N3_SEND_PERIOD_MS);

    while (true) {
        sensor_packet_t pkt = {0};
        snprintf(pkt.node_id, sizeof(pkt.node_id), "%s", NODE_ID);
        fill_src_id(pkt.src_mac);
        pkt.start_timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        pkt.batch_id = ++rtc_batch_id;
        fill_synthetic_batch(&pkt);

        thread_app_packet_t tp = {0};
        tp.magic = THREAD_PACKET_MAGIC;
        tp.version = THREAD_PACKET_VERSION;
        tp.batch_id = pkt.batch_id;

        size_t enc_len = 0;
        t_stage = esp_timer_get_time();
        ret = crypto_encrypt_payload(
            (const uint8_t *)&pkt,
            sizeof(pkt),
            pkt.batch_id,
            pkt.src_mac,
            tp.iv,
            tp.encrypted_payload,
            sizeof(tp.encrypted_payload),
            &enc_len,
            tp.auth_tag
        );
        int64_t t_crypto_ms = (esp_timer_get_time() - t_stage) / 1000;

        if (ret == ESP_OK) {
            tp.encrypted_len = (uint16_t)enc_len;

            t_stage = esp_timer_get_time();
            ret = thread_sender_send_packet(&tp);
            int64_t t_send_ms = (esp_timer_get_time() - t_stage) / 1000;

            ESP_LOGI(TAG,
                     "N3 batch=%u ret=%s crypto_ms=%lld send_ms=%lld",
                     pkt.batch_id,
                     esp_err_to_name(ret),
                     t_crypto_ms,
                     t_send_ms);
        } else {
            ESP_LOGE(TAG, "N3 crypto fallo: %s", esp_err_to_name(ret));
        }

        /*
         * N3 permanece activo como Router para reenviar paquetes de N1/N2.
         * Este delay solo controla el periodo de su propio dato sintético.
         */
        vTaskDelay(pdMS_TO_TICKS(N3_SEND_PERIOD_MS));
    }
}
