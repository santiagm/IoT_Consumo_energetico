#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "config.h"
#include "telemetry_packet.h"
#include "zigbee_sender.h"
#include "app_crypto.h"

static const char *TAG = "ROUTER_N3_INSTALL_CODE";

static uint16_t s_batch_id = 0;
static uint32_t s_packet_id = 0;

#define ROUTER_TX_INTERVAL_MS        5000  /* Enviar telemetría cada 5 segundos */

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
        snprintf(out,
                 18,
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 ieee[7], ieee[6], ieee[5], ieee[4], ieee[3], ieee[2]);
    } else {
        snprintf(out, 18, "00:00:00:00:00:00");
    }
}

static void fill_synthetic_samples(synthetic_sample_t samples[STREAM_SAMPLE_COUNT], uint16_t batch_id)
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
                 "IEEE802154 real del router para config.h gateway = {0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X}",
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

static void router_telemetry_task(void *pv)
{
    (void)pv;
    
    ESP_LOGI(TAG, "Iniciando tarea de telemetría del Router N3");
    
    /* Esperar a que se una a la red */
    int wait_count = 0;
    while (!zigbee_sender_is_joined()) {
        ESP_LOGW(TAG, "Router esperando unirse a red Zigbee...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        wait_count++;
        if (wait_count > 120) {  /* Timeout 2 minutos */
            ESP_LOGE(TAG, "Router timeout esperando unirse a red");
            break;
        }
    }
    
    if (zigbee_sender_is_joined()) {
        ESP_LOGI(TAG, "Router N3 unido a red Zigbee, iniciando envío de telemetría periódica");
    }
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(ROUTER_TX_INTERVAL_MS));
        
        if (!zigbee_sender_is_joined()) {
            ESP_LOGW(TAG, "Router no unido, saltando envío de telemetría");
            continue;
        }
        
        sensor_packet_t packet = {0};
        secure_zigbee_packet_t secure_packet = {0};
        
        packet.start_timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        packet.batch_id = ++s_batch_id;
        packet.packet_id = ++s_packet_id;
        strncpy(packet.node_id, NODE_ID, sizeof(packet.node_id) - 1);
        fill_src_mac(packet.src_mac);
        
        ESP_LOGI(TAG,
                 "Construyendo sensor_packet_t N3 packet_id=%u src_mac=%s batch=%u",
                 (unsigned)packet.packet_id,
                 packet.src_mac,
                 (unsigned)packet.batch_id);
        
        fill_synthetic_samples(packet.muestras, packet.batch_id);
        
        if (app_crypto_encrypt_sensor_packet(&packet, &secure_packet) == ESP_OK) {
            if (zigbee_sender_send_packet(&secure_packet) == ESP_OK) {
                ESP_LOGI(TAG,
                         "Telemetría N3 enviada packet_id=%u batch=%u",
                         (unsigned)packet.packet_id,
                         (unsigned)packet.batch_id);
            } else {
                ESP_LOGW(TAG, "Error enviando telemetría N3 packet_id=%u", (unsigned)packet.packet_id);
            }
        } else {
            ESP_LOGW(TAG, "Error cifrando telemetría N3 packet_id=%u", (unsigned)packet.packet_id);
        }
    }
    
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGW(TAG, "Iniciando firmware_router_zigbee_install_code N3 (Router, sin deep sleep)");
    
#if NODE_DEBUG_LOGS
    print_ieee802154_mac();
#endif
    
    init_nvs_all();
    
    ESP_ERROR_CHECK(zigbee_sender_start());
    
    /* Crear tarea para enviar telemetría periódicamente */
    BaseType_t ok = xTaskCreate(
        router_telemetry_task,
        "Router_Telemetry",
        8192,
        NULL,
        4,
        NULL
    );
    
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "No se pudo crear tarea de telemetría del router");
    }
    
    /* El router permanece activo indefinidamente.
     * Además imprime periódicamente vecinos/children para verificar que N1 y N2
     * quedaron adjuntos a N3 como Sleepy End Devices.
     */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(ROUTER_NETWORK_TABLE_LOG_MS));

        if (!zigbee_sender_is_joined()) {
            ESP_LOGW(TAG, "Router N3 activo, pero todavía no unido a la red Zigbee");
            continue;
        }

        uint8_t sed_children = zigbee_sender_print_network_tables();

        if (sed_children < ROUTER_EXPECTED_SED_CHILDREN) {
            ESP_LOGW(TAG,
                     "N3 todavía no tiene los %u SED esperados como hijos directos. Hijos SED detectados=%u. Reabriendo permit-join desde N3 %u s",
                     (unsigned)ROUTER_EXPECTED_SED_CHILDREN,
                     (unsigned)sed_children,
                     (unsigned)ROUTER_PERMIT_JOIN_SECONDS);
            (void)zigbee_sender_open_router_join_window(ROUTER_PERMIT_JOIN_SECONDS);
        } else {
            ESP_LOGI(TAG,
                     "OK Mesh: N3 tiene %u Sleepy End Devices hijos directos; N1/N2 deberían estar pasando por el Router",
                     (unsigned)sed_children);
        }
    }
}
