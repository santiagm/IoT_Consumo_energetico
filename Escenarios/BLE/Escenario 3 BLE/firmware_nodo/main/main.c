#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs_flash.h"

#include "driver/gpio.h"

#include "config.h"
#include "telemetry_packet.h"
#include "crypto_helper.h"
#include "ble_broadcaster.h"

static const char *TAG = "NODO_BLE_ESC3";

RTC_DATA_ATTR static uint16_t s_batch_id = 0;

static void fill_src_mac(char out_mac[18])
{
    uint8_t mac[6] = {0};

    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    snprintf(out_mac, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static uint32_t get_timestamp_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
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

static void prepare_gpios_for_deep_sleep(void)
{
#ifdef RGB_LED_GPIO
    set_gpio_input_floating(RGB_LED_GPIO);
#endif

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
}

static void configure_sleep_sources(void)
{
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

#if NODE_ENABLE_RTC_PERIODIC_WAKEUP
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(
        (uint64_t)NODE_WAKEUP_PERIOD_SEC * 1000000ULL));
#endif
}

static void enter_deep_sleep(void)
{
    prepare_gpios_for_deep_sleep();
    configure_sleep_sources();

#if NODE_DEBUG_LOGS
    ESP_LOGI(TAG, "Entrando en deep sleep. RTC=%d s", NODE_WAKEUP_PERIOD_SEC);
#endif

    esp_deep_sleep_start();
}

static int16_t synthetic_rand_i16(int16_t min_value, int16_t max_value)
{
    uint32_t span = (uint32_t)((int32_t)max_value - (int32_t)min_value + 1);
    return (int16_t)((int32_t)min_value + (int32_t)(esp_random() % span));
}

static esp_err_t fill_synthetic_packet(sensor_packet_t *packet)
{
    if (packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(packet, 0, sizeof(*packet));

    fill_src_mac(packet->src_mac);
    packet->start_timestamp_ms = get_timestamp_ms();
    packet->batch_id = s_batch_id;

    for (uint8_t i = 0; i < STREAM_SAMPLE_COUNT; i++) {
        packet->muestras[i].ax = synthetic_rand_i16(-4096, 4096);
        packet->muestras[i].ay = synthetic_rand_i16(-4096, 4096);
        packet->muestras[i].az = synthetic_rand_i16(12000, 18000);
        packet->muestras[i].gx = synthetic_rand_i16(-500, 500);
        packet->muestras[i].gy = synthetic_rand_i16(-500, 500);
        packet->muestras[i].gz = synthetic_rand_i16(-500, 500);
    }

#if NODE_DEBUG_LOGS
    ESP_LOGI(TAG,
             "Sintetico aleatorio raw[0]: ax=%d ay=%d az=%d gx=%d gy=%d gz=%d",
             packet->muestras[0].ax,
             packet->muestras[0].ay,
             packet->muestras[0].az,
             packet->muestras[0].gx,
             packet->muestras[0].gy,
             packet->muestras[0].gz);
#endif

    return ESP_OK;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

#if NODE_DEBUG_LOGS
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Wakeup reason: %d", wakeup_reason);
#endif

    sensor_packet_t packet;
    err = fill_synthetic_packet(&packet);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fallo generando datos sinteticos: %s", esp_err_to_name(err));
        enter_deep_sleep();
    }

    uint8_t encrypted_payload[sizeof(sensor_packet_t)];
    uint8_t auth_tag[AES_GCM_TAG_LEN];

    memset(encrypted_payload, 0, sizeof(encrypted_payload));
    memset(auth_tag, 0, sizeof(auth_tag));

    err = crypto_encrypt_payload((const uint8_t *)&packet,
                                 sizeof(packet),
                                 packet.batch_id,
                                 encrypted_payload,
                                 auth_tag);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error cifrando datos: %s", esp_err_to_name(err));
        enter_deep_sleep();
    }

    err = ble_broadcaster_init();
    if (err == ESP_OK) {
        err = ble_broadcaster_send_packet(packet.batch_id,
                                          encrypted_payload,
                                          sizeof(encrypted_payload),
                                          auth_tag);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error enviando BLE: %s", esp_err_to_name(err));
    }

    ble_broadcaster_stop();

#if NODE_DEBUG_LOGS
    ESP_LOGI(TAG,
             "Paquete enviado. src_mac=%s batch_id=%u timestamp=%" PRIu32,
             packet.src_mac,
             packet.batch_id,
             packet.start_timestamp_ms);
#endif

    s_batch_id++;

    enter_deep_sleep();
}
