#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "config.h"
#include "thread_packet.h"
#include "telemetry_packet.h"
#include "crypto_helper.h"
#include "thread_receiver.h"
#include "telemetry_json.h"
#include "uart_bridge_sender.h"


static const char *TAG = "GW_THREAD_UART";

typedef struct {
    char node_id[8];
    uint16_t last_batch_id;
    bool valid;
} node_dup_entry_t;

#define DUP_TABLE_SIZE 10
static node_dup_entry_t s_dup_table[DUP_TABLE_SIZE];

static uint32_t s_packets_received = 0;
static uint32_t s_packets_uart = 0;
static uint32_t s_packets_duplicates = 0;
static uint32_t s_packets_invalid = 0;
static uint32_t s_decrypt_errors = 0;
static uint32_t s_uart_errors = 0;

static esp_err_t gateway_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static bool is_duplicate_packet(const char *node_id, uint16_t batch_id)
{
    if (!node_id || node_id[0] == '\0') {
        node_id = "UNKNOWN";
    }

    int free_idx = -1;

    for (int i = 0; i < DUP_TABLE_SIZE; ++i) {
        if (s_dup_table[i].valid) {
            if (strncmp(s_dup_table[i].node_id, node_id, sizeof(s_dup_table[i].node_id)) == 0) {
                if (s_dup_table[i].last_batch_id == batch_id) {
                    return true;
                }
                s_dup_table[i].last_batch_id = batch_id;
                return false;
            }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }

    if (free_idx < 0) {
        free_idx = 0;
    }

    memset(&s_dup_table[free_idx], 0, sizeof(s_dup_table[free_idx]));
    strncpy(s_dup_table[free_idx].node_id, node_id, sizeof(s_dup_table[free_idx].node_id) - 1);
    s_dup_table[free_idx].last_batch_id = batch_id;
    s_dup_table[free_idx].valid = true;

    return false;
}

static bool validate_thread_packet(const thread_app_packet_t *packet)
{
    if (packet == NULL) {
        return false;
    }

    if (packet->magic != THREAD_PACKET_MAGIC) {
        ESP_LOGW(TAG, "Magic invalido: 0x%04X", packet->magic);
        return false;
    }

    if (packet->version != THREAD_PACKET_VERSION) {
        ESP_LOGW(TAG, "Version invalida: %u", packet->version);
        return false;
    }

    if (packet->encrypted_len != sizeof(sensor_packet_t)) {
        ESP_LOGW(TAG, "encrypted_len invalido: %u esperado=%u",
                 packet->encrypted_len, (unsigned)sizeof(sensor_packet_t));
        return false;
    }

    return true;
}

static void log_metrics(void)
{
    ESP_LOGI(TAG,
             "Metricas RX=%" PRIu32 " UART=%" PRIu32 " DUP=%" PRIu32
             " INV=%" PRIu32 " DEC_ERR=%" PRIu32 " UART_ERR=%" PRIu32,
             s_packets_received,
             s_packets_uart,
             s_packets_duplicates,
             s_packets_invalid,
             s_decrypt_errors,
             s_uart_errors);
}

static void handle_thread_packet(const thread_app_packet_t *packet, int rssi, int lqi, void *ctx)
{
    (void)ctx;
    s_packets_received++;

    if (!validate_thread_packet(packet)) {
        s_packets_invalid++;
        log_metrics();
        return;
    }

    ESP_LOGI(TAG, "Paquete Thread valido recibido. batch_id=%u", packet->batch_id);

    sensor_packet_t sensor_packet = {0};
    size_t plain_len = 0;

    esp_err_t ret = crypto_decrypt_payload(packet->encrypted_payload,
                                           packet->encrypted_len,
                                           packet->iv,
                                           packet->auth_tag,
                                           (uint8_t *)&sensor_packet,
                                           sizeof(sensor_packet),
                                           &plain_len);
    if (ret != ESP_OK || plain_len != sizeof(sensor_packet_t)) {
        s_decrypt_errors++;
        ESP_LOGE(TAG, "Error descifrando AES-GCM. batch_id=%u", packet->batch_id);
        log_metrics();
        return;
    }

    if (sensor_packet.batch_id != packet->batch_id) {
        ESP_LOGW(TAG, "batch_id no coincide: header=%u payload=%u",
                 packet->batch_id, sensor_packet.batch_id);
    }

    bool duplicate = is_duplicate_packet(sensor_packet.node_id, sensor_packet.batch_id);
    if (duplicate) {
        s_packets_duplicates++;
        ESP_LOGW(TAG, "Duplicado Thread detectado node_id=%s batch_id=%u",
                 sensor_packet.node_id, sensor_packet.batch_id);
        log_metrics();
        return;
    }

    uint32_t ts_gateway_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    /* RSSI llega desde el callback UDP nativo de OpenThread.
     * LQI queda en -1 si la versión/API no lo expone de forma portable.
     */
    if (rssi == 127) {
        rssi = -127;
    }

    char json[512];
    ret = telemetry_json_build(&sensor_packet,
                               ts_gateway_ms,
                               duplicate,
                               true,
                               rssi,
                               lqi,
                               json,
                               sizeof(json));
    if (ret != ESP_OK) {
        s_packets_invalid++;
        ESP_LOGE(TAG, "No se pudo construir JSON. node_id=%s batch_id=%u",
                 sensor_packet.node_id, packet->batch_id);
        log_metrics();
        return;
    }

    ESP_LOGI(TAG, "JSON generado: %s", json);

    ret = uart_bridge_send_json_line(json);
    if (ret != ESP_OK) {
        s_uart_errors++;
        ESP_LOGE(TAG, "Error enviando JSON por UART batch=%u", packet->batch_id);
        log_metrics();
        return;
    }

    s_packets_uart++;
    ESP_LOGI(TAG, "JSON enviado a Gateway MQTT por UART batch=%u", packet->batch_id);
    log_metrics();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando Thread Gateway UART");

    ESP_ERROR_CHECK(gateway_nvs_init());
    ESP_ERROR_CHECK(uart_bridge_sender_init());

    ESP_LOGI(TAG, "Iniciando receptor Thread/OpenThread...");
    esp_err_t thread_ret = thread_receiver_init();

    if (thread_ret != ESP_OK) {
        ESP_LOGE(TAG, "Thread no se adjunto a tiempo. Reiniciando en 5 segundos...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    ESP_ERROR_CHECK(thread_receiver_start(handle_thread_packet, NULL));

    ESP_LOGI(TAG, "Thread Gateway listo. UDP Thread=%d UART TX=%d RX=%d baud=%d",
             THREAD_UDP_PORT, UART_BRIDGE_TX_GPIO, UART_BRIDGE_RX_GPIO, UART_BRIDGE_BAUD);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        log_metrics();
    }
}
