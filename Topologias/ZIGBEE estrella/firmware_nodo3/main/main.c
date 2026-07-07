#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "config.h"
#include "telemetry_packet.h"
#include "zigbee_sender.h"
#include "app_crypto.h"

static const char *TAG = "NODO_INSTALL_CODE";
RTC_DATA_ATTR static uint16_t s_batch_id = 0;

static void init_nvs_all(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(nvs_flash_init_partition("zb_storage"));
}

static void fill_src_mac(char out[18])
{
    uint8_t ieee[8] = {0};
    esp_err_t err = esp_read_mac(ieee, ESP_MAC_IEEE802154);

    if (err == ESP_OK) {
        /* El paquete lógico usa char[18], por eso se usa formato MAC de 6 bytes.
         * Se toma la parte estable del IEEE802.15.4 en orden usado por los escenarios previos.
         */
        snprintf(out,
                 18,
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 ieee[7], ieee[6], ieee[5], ieee[4], ieee[3], ieee[2]);
    } else {
        snprintf(out, 18, "00:00:00:00:00:00");
    }
}

static void fill_node_id(char out[SENSOR_NODE_ID_LEN])
{
    snprintf(out, SENSOR_NODE_ID_LEN, "%s", NODE_ID);
}

static void enter_deep_sleep(void)
{
    ESP_LOGW(TAG, "Entrando en deep sleep");
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(NODE_SLEEP_TIME_US));
    esp_deep_sleep_start();
}


static void fill_synthetic_samples(mpu_sample_t samples[STREAM_SAMPLE_COUNT], uint16_t batch_id)
{
    for (uint8_t i = 0; i < STREAM_SAMPLE_COUNT; i++) {
        samples[i].ax = (int16_t)(1000 + batch_id + i);
        samples[i].ay = (int16_t)(-500 + batch_id + i);
        samples[i].az = (int16_t)(16384 + i);
        samples[i].gx = (int16_t)(10 + batch_id + i);
        samples[i].gy = (int16_t)(-20 + batch_id + i);
        samples[i].gz = (int16_t)(30 + batch_id + i);
    }
}

static void print_ieee802154_mac(void)
{
    uint8_t ieee[8] = {0};

    esp_err_t err = esp_read_mac(ieee, ESP_MAC_IEEE802154);

    if (err == ESP_OK) {
        ESP_LOGI("NODE_IEEE",
                 "IEEE802154 real del nodo para config.h gateway = {0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X}",
                 ieee[0],
                 ieee[1],
                 ieee[2],
                 ieee[3],
                 ieee[4],
                 ieee[5],
                 ieee[6],
                 ieee[7]);

        ESP_LOGI("NODE_IEEE",
                 "IEEE802154 string = %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                 ieee[0],
                 ieee[1],
                 ieee[2],
                 ieee[3],
                 ieee[4],
                 ieee[5],
                 ieee[6],
                 ieee[7]);
    } else {
        ESP_LOGE("NODE_IEEE",
                 "No se pudo leer ESP_MAC_IEEE802154 err=%s",
                 esp_err_to_name(err));
    }
}


void app_main(void)
{
    int64_t t0 = esp_timer_get_time();
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGW(TAG, "Iniciando firmware_nodo3_zigbee_estrella node_id=N3");
#if NODE_DEBUG_LOGS
    print_ieee802154_mac();
#endif
    ESP_LOGW(TAG, "Wakeup cause=%d", (int)cause);

    init_nvs_all();

    sensor_packet_t packet = {0};
    secure_zigbee_packet_t secure_packet = {0};

    packet.start_timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    packet.batch_id = ++s_batch_id;
    fill_node_id(packet.node_id);
    fill_src_mac(packet.src_mac);

#if NODE_DEBUG_LOGS
    ESP_LOGI(TAG,
             "Construyendo sensor_packet_t node_id=%s src_mac=%s timestamp=%" PRIu32 " batch=%u muestras=%u",
             packet.node_id,
             packet.src_mac,
             packet.start_timestamp_ms,
             (unsigned)packet.batch_id,
             (unsigned)STREAM_SAMPLE_COUNT);
#endif

    // Generación de datos sintéticos sin sensor físico
    int64_t t_stage = esp_timer_get_time();
    fill_synthetic_samples(packet.muestras, packet.batch_id);
    int64_t t_synth_ms = (esp_timer_get_time() - t_stage) / 1000;

    // Medici\u00f3n AES-GCM
    t_stage = esp_timer_get_time();
    ESP_ERROR_CHECK(app_crypto_encrypt_sensor_packet(&packet, &secure_packet));
    int64_t t_crypto_ms = (esp_timer_get_time() - t_stage) / 1000;

    // Medici\u00f3n Zigbee start + join
    t_stage = esp_timer_get_time();
    ESP_ERROR_CHECK(zigbee_sender_start());

    while (!zigbee_sender_is_joined()) {
#if NODE_DEBUG_LOGS
        ESP_LOGW(TAG, "Esperando union Zigbee...");
#endif
        vTaskDelay(pdMS_TO_TICKS(ZIGBEE_JOIN_POLL_MS));
    }
    int64_t t_zigbee_join_ms = (esp_timer_get_time() - t_stage) / 1000;

    // Medici\u00f3n Zigbee send
    t_stage = esp_timer_get_time();
#if NODE_DEBUG_LOGS
    ESP_LOGW(TAG, "Nodo Zigbee unido, enviando paquete");
#endif
    ESP_ERROR_CHECK(zigbee_sender_send_packet(&secure_packet));
    int64_t t_zigbee_send_ms = (esp_timer_get_time() - t_stage) / 1000;

    vTaskDelay(pdMS_TO_TICKS(POST_ZIGBEE_TX_DELAY_MS));

    int64_t t_total_ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG,
             "batch=%u activo_ms=%lld [synth_ms=%lld crypto_ms=%lld zigbee_join_ms=%lld zigbee_send_ms=%lld post_tx_ms=%u]",
             (unsigned)packet.batch_id,
             t_total_ms,
             t_synth_ms,
             t_crypto_ms,
             t_zigbee_join_ms,
             t_zigbee_send_ms,
             (unsigned)POST_ZIGBEE_TX_DELAY_MS);

    zigbee_sender_deinit_before_sleep();

    enter_deep_sleep();
}
