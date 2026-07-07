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
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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
    int64_t t0 = esp_timer_get_time();
    int64_t t_stage = 0;

    esp_sleep_wakeup_cause_t causa = esp_sleep_get_wakeup_cause();

#if NODE_DEBUG_LOGS
    ESP_LOGI(TAG, "Wakeup cause=%d", causa);

    if (causa == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "Desperto por TIMER RTC");
    } else if (causa == ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGW(TAG, "Arranque normal o reset, NO fue wakeup por deep sleep timer");
    } else {
        ESP_LOGW(TAG, "Otra causa de wakeup=%d", causa);
    }
#endif

    esp_err_t ret = init_nvs();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init fallo: %s", esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(2000));
        deep_sleep_now();
    }

    sensor_packet_t pkt = {0};

    snprintf(pkt.node_id, sizeof(pkt.node_id), "%s", NODE_ID);
    fill_src_id(pkt.src_mac);

    pkt.start_timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    pkt.batch_id = ++rtc_batch_id;

#if NODE_DEBUG_LOGS
    ESP_LOGI(
        TAG,
        "Nodo Thread inicio batch=%u src=%s",
        pkt.batch_id,
        pkt.src_mac
    );
#endif

    t_stage = esp_timer_get_time();
    fill_synthetic_batch(&pkt);
    int64_t t_synth_ms = (esp_timer_get_time() - t_stage) / 1000;

#if NODE_DEBUG_LOGS
    ESP_LOGI(
        TAG,
        "Datos sinteticos OK batch=%u tiempo_synth_ms=%lld",
        pkt.batch_id,
        t_synth_ms
    );
#endif

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

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "crypto_encrypt_payload fallo: %s tiempo_crypto_ms=%lld", esp_err_to_name(ret), t_crypto_ms);
        vTaskDelay(pdMS_TO_TICKS(1000));
        deep_sleep_now();
    }

    tp.encrypted_len = (uint16_t)enc_len;

#if NODE_DEBUG_LOGS
    ESP_LOGI(
        TAG,
        "Payload cifrado OK batch=%u encrypted_len=%u tiempo_crypto_ms=%lld",
        tp.batch_id,
        tp.encrypted_len,
        t_crypto_ms
    );
#endif

    t_stage = esp_timer_get_time();
    //vTaskDelay(pdMS_TO_TICKS(100));
    ret = thread_sender_init();
    int64_t t_thread_init_ms = (esp_timer_get_time() - t_stage) / 1000;

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "thread_sender_init fallo: %s tiempo_thread_init_ms=%lld", esp_err_to_name(ret), t_thread_init_ms);
        vTaskDelay(pdMS_TO_TICKS(1000));
        deep_sleep_now();
    }

    t_stage = esp_timer_get_time();
    ret = thread_sender_send_packet(&tp);
    int64_t t_thread_send_ms = (esp_timer_get_time() - t_stage) / 1000;

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "thread_sender_send_packet fallo: %s tiempo_thread_send_ms=%lld", esp_err_to_name(ret), t_thread_send_ms);
    } else {
#if NODE_DEBUG_LOGS
        ESP_LOGI(TAG, "Paquete Thread enviado OK batch=%u tiempo_thread_send_ms=%lld", tp.batch_id, t_thread_send_ms);
#endif
    }

    int64_t t_total_ms = (esp_timer_get_time() - t0) / 1000;
    
    ESP_LOGI(
        TAG,
        "batch=%u ret=%s activo_ms=%lld [synth_ms=%lld crypto_ms=%lld thread_init_ms=%lld thread_send_ms=%lld]",
        pkt.batch_id,
        esp_err_to_name(ret),
        t_total_ms,
        t_synth_ms,
        t_crypto_ms,
        t_thread_init_ms,
        t_thread_send_ms
    );

    /*
     * Margen post-envio Thread.
     * ret=ESP_OK solo indica que OpenThread acepto el paquete,
     * no garantiza que el gateway ya lo recibio/proceso.
     * Reducido de 500ms a 200ms para optimizar tiempo activo.
     */
    ESP_LOGI(TAG, "Esperando margen post-envio Thread antes de dormir");
    vTaskDelay(pdMS_TO_TICKS(50));


    deep_sleep_now();
}