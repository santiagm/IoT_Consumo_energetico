#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "security_payload.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "driver/uart.h"
#include "psa/crypto.h"

static const char *TAG = "GW_ESPNOW_UART";

/*
 * ============================================================
 * ESCENARIO 3 - TOPOLOGIA ESTRELLA - GATEWAY ESPNOW -> UART
 * ============================================================
 *
 * Recibe paquetes de varios nodos ESP-NOW, manteniendo:
 * - Seguridad nativa ESP-NOW con PMK/LMK.
 * - AES-128-GCM de aplicacion sobre sensor_packet_t.
 * - STREAM_SAMPLE_COUNT y tiempos del Escenario 3.
 *
 * Este firmware NO publica a ThingsBoard.
 * Solo genera una linea JSON por UART para que MQTT_Gateway la publique.
 */

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

typedef struct {
    const char *node_id;
    uint8_t mac[ESP_NOW_ETH_ALEN];
    bool last_batch_valid;
    uint16_t last_batch_id;
} node_peer_t;

typedef struct {
    char node_id[8];
    char src_mac[18];
    sensor_packet_t packet;
    int rssi;
    int lqi;
    bool duplicate;
    bool decrypt_ok;
} received_packet_t;

_Static_assert(sizeof(sensor_packet_t) <= ESP_NOW_MAX_DATA_LEN,
               "sensor_packet_t exceeds ESP-NOW payload limit. Reduce STREAM_SAMPLE_COUNT");

_Static_assert(sizeof(secure_espnow_packet_t) <= ESP_NOW_MAX_DATA_LEN,
               "secure_espnow_packet_t exceeds ESP-NOW payload limit. Reduce STREAM_SAMPLE_COUNT");

static const uint8_t ESPNOW_PMK[16] = ESPNOW_PMK_BYTES;
static const uint8_t ESPNOW_LMK[16] = ESPNOW_LMK_BYTES;
static const uint8_t s_app_aes_key[16] = APP_AES_GCM_KEY_BYTES;

static node_peer_t s_nodes[] = {
    {.node_id = NODE1_ID, .mac = NODE1_PEER_MAC_BYTES},
    {.node_id = NODE2_ID, .mac = NODE2_PEER_MAC_BYTES},
    {.node_id = NODE3_ID, .mac = NODE3_PEER_MAC_BYTES},
};

static QueueHandle_t s_packet_queue = NULL;

static uint32_t get_timestamp_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static int rssi_to_quality_255(int8_t rssi)
{
    if (rssi <= -100) {
        return 0;
    }

    if (rssi >= -50) {
        return 255;
    }

    return ((int)rssi + 100) * 255 / 50;
}

static void mac_to_string(const uint8_t *mac, char *mac_str, size_t mac_str_len)
{
    snprintf(mac_str, mac_str_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool is_zero_mac(const uint8_t *mac)
{
    for (size_t i = 0; i < ESP_NOW_ETH_ALEN; i++) {
        if (mac[i] != 0U) {
            return false;
        }
    }
    return true;
}

static node_peer_t *find_node_by_mac(const uint8_t *mac)
{
    for (size_t i = 0; i < sizeof(s_nodes) / sizeof(s_nodes[0]); i++) {
        if (memcmp(s_nodes[i].mac, mac, ESP_NOW_ETH_ALEN) == 0) {
            return &s_nodes[i];
        }
    }
    return NULL;
}

static bool update_duplicate_state(node_peer_t *node, uint16_t batch_id)
{
    if (node == NULL) {
        return false;
    }

    bool duplicate = node->last_batch_valid && (node->last_batch_id == batch_id);
    node->last_batch_id = batch_id;
    node->last_batch_valid = true;
    return duplicate;
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

static esp_err_t decrypt_secure_packet_gcm(const secure_espnow_packet_t *secure_packet,
                                           sensor_packet_t *plain_packet)
{
    if (secure_packet == NULL || plain_packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (secure_packet->magic != ESPNOW_APP_SEC_MAGIC) {
        ESP_LOGW(TAG, "Magic invalido: 0x%08" PRIX32, secure_packet->magic);
        return ESP_FAIL;
    }

    if (secure_packet->version != ESPNOW_APP_SEC_VERSION) {
        ESP_LOGW(TAG, "Version invalida: %u", (unsigned)secure_packet->version);
        return ESP_ERR_INVALID_ARG;
    }

    if (secure_packet->encrypted_len != sizeof(sensor_packet_t)) {
        ESP_LOGW(TAG,
                 "encrypted_len invalido: %u esperado=%u",
                 (unsigned)secure_packet->encrypted_len,
                 (unsigned)sizeof(sensor_packet_t));
        return ESP_ERR_INVALID_SIZE;
    }

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
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
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

    uint8_t aead_input[sizeof(sensor_packet_t) + AES_GCM_TAG_LEN] = {0};
    memcpy(aead_input, secure_packet->encrypted_payload, sizeof(sensor_packet_t));
    memcpy(&aead_input[sizeof(sensor_packet_t)], secure_packet->auth_tag, AES_GCM_TAG_LEN);

    memset(plain_packet, 0, sizeof(*plain_packet));
    size_t plain_len = 0;

    status = psa_aead_decrypt(key_id,
                              PSA_ALG_GCM,
                              secure_packet->iv,
                              AES_GCM_IV_LEN,
                              (const uint8_t *)&aad,
                              sizeof(aad),
                              aead_input,
                              sizeof(aead_input),
                              (uint8_t *)plain_packet,
                              sizeof(*plain_packet),
                              &plain_len);

    (void)psa_destroy_key(key_id);

    if (status != PSA_SUCCESS) {
        memset(plain_packet, 0, sizeof(*plain_packet));
        ESP_LOGW(TAG, "AES-128-GCM autenticacion fallida, paquete descartado PSA: %ld", (long)status);
        return ESP_FAIL;
    }

    if (plain_len != sizeof(sensor_packet_t)) {
        memset(plain_packet, 0, sizeof(*plain_packet));
        ESP_LOGW(TAG,
                 "AES-GCM plain_len invalido: %u esperado=%u",
                 (unsigned)plain_len,
                 (unsigned)sizeof(sensor_packet_t));
        return ESP_FAIL;
    }

    if (plain_packet->batch_id != secure_packet->batch_id) {
        ESP_LOGW(TAG,
                 "batch_id no coincide tras descifrar: header=%u payload=%u",
                 (unsigned)secure_packet->batch_id,
                 (unsigned)plain_packet->batch_id);
        memset(plain_packet, 0, sizeof(*plain_packet));
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t init_nvs_storage(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }

    return err;
}

static esp_err_t init_uart_to_mqtt_gateway(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM,
                                 UART_TX_GPIO,
                                 UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG,
             "UART hacia MQTT_Gateway listo: UART%d TX=%d RX=%d baud=%d",
             UART_PORT_NUM,
             UART_TX_GPIO,
             UART_RX_GPIO,
             UART_BAUDRATE);
    return ESP_OK;
}

static esp_err_t wifi_init_sta_radio_only(void)
{
    esp_err_t err = init_nvs_storage();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGW(TAG, "No se pudo crear default WIFI STA o ya existe");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
        return err;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

    uint8_t gw_mac[ESP_NOW_ETH_ALEN] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, gw_mac) == ESP_OK) {
        ESP_LOGI(TAG,
                 "MAC STA de este ESPNOW_Gateway: %02X:%02X:%02X:%02X:%02X:%02X",
                 gw_mac[0], gw_mac[1], gw_mac[2], gw_mac[3], gw_mac[4], gw_mac[5]);
        ESP_LOGI(TAG, "Esta MAC debe ir en GATEWAY_MAC_BYTES de firmware_nodo1/2/3");
    }

    ESP_LOGI(TAG, "WiFi STA radio-only listo en canal ESP-NOW=%d", ESPNOW_CHANNEL);
    return ESP_OK;
}

static esp_err_t espnow_add_secure_peer(const node_peer_t *node)
{
    if (node == NULL || is_zero_mac(node->mac)) {
        return ESP_OK;
    }

    esp_now_peer_info_t peer_info;
    memset(&peer_info, 0, sizeof(peer_info));

    memcpy(peer_info.peer_addr, node->mac, ESP_NOW_ETH_ALEN);
    memcpy(peer_info.lmk, ESPNOW_LMK, sizeof(peer_info.lmk));
    peer_info.channel = ESPNOW_CHANNEL;
    peer_info.ifidx = WIFI_IF_STA;
    peer_info.encrypt = true;

    esp_err_t err;
    if (esp_now_is_peer_exist(node->mac)) {
        err = esp_now_mod_peer(&peer_info);
    } else {
        err = esp_now_add_peer(&peer_info);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error agregando peer cifrado %s: %s", node->node_id, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "Peer cifrado %s listo: %02X:%02X:%02X:%02X:%02X:%02X",
             node->node_id,
             node->mac[0], node->mac[1], node->mac[2],
             node->mac[3], node->mac[4], node->mac[5]);

    return ESP_OK;
}

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data,
                           int len)
{
    if (info == NULL || data == NULL) {
        ESP_LOGW(TAG, "Callback ESP-NOW sin info/data");
        return;
    }

    if (len != sizeof(secure_espnow_packet_t)) {
        ESP_LOGW(TAG,
                 "Paquete seguro invalido, tamano %d esperado %d",
                 len,
                 (int)sizeof(secure_espnow_packet_t));
        return;
    }

    if (s_packet_queue == NULL) {
        ESP_LOGW(TAG, "Cola no inicializada, paquete descartado");
        return;
    }

    node_peer_t *node = find_node_by_mac(info->src_addr);
    const secure_espnow_packet_t *secure_packet = (const secure_espnow_packet_t *)data;

    received_packet_t received_packet;
    memset(&received_packet, 0, sizeof(received_packet));
    strlcpy(received_packet.node_id, node != NULL ? node->node_id : "UNKNOWN", sizeof(received_packet.node_id));
    mac_to_string(info->src_addr, received_packet.src_mac, sizeof(received_packet.src_mac));
    received_packet.rssi = (info->rx_ctrl != NULL) ? info->rx_ctrl->rssi : 0;
    received_packet.lqi = (info->rx_ctrl != NULL) ? rssi_to_quality_255((int8_t)received_packet.rssi) : -1;
    received_packet.decrypt_ok = false;

    esp_err_t err = decrypt_secure_packet_gcm(secure_packet, &received_packet.packet);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Paquete descartado por validacion AES-GCM: %s", esp_err_to_name(err));
        return;
    }

    received_packet.decrypt_ok = true;

    if (received_packet.packet.src_mac[0] != '\0') {
        strlcpy(received_packet.src_mac,
                received_packet.packet.src_mac,
                sizeof(received_packet.src_mac));
    }

    received_packet.duplicate = update_duplicate_state(node, received_packet.packet.batch_id);

    if (xQueueSend(s_packet_queue, &received_packet, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Cola llena, paquete descartado");
    }
}

static esp_err_t espnow_init_secure_star(void)
{
    esp_err_t err = esp_now_init();
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "Error esp_now_init: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_now_set_pmk(ESPNOW_PMK);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando PMK: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < sizeof(s_nodes) / sizeof(s_nodes[0]); i++) {
        err = espnow_add_secure_peer(&s_nodes[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = esp_now_register_recv_cb(espnow_recv_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error registrando callback ESP-NOW: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "ESP-NOW estrella seguro listo en canal %d", ESPNOW_CHANNEL);
    return ESP_OK;
}

static void build_and_send_required_json(const received_packet_t *received_packet)
{
    if (received_packet == NULL) {
        return;
    }

    const sensor_packet_t *packet = &received_packet->packet;
    const mpu_sample_t *sample = &packet->muestras[0];

    const uint32_t ts_gateway_ms = get_timestamp_ms();
    const uint32_t node_tx_timestamp_ms = packet->start_timestamp_ms;
    const uint32_t latency_ms = (ts_gateway_ms >= node_tx_timestamp_ms)
                                ? (ts_gateway_ms - node_tx_timestamp_ms)
                                : 0U;

    const double synthetic_ax = sample->ax / 16384.0;
    const double synthetic_ay = sample->ay / 16384.0;
    const double synthetic_az = sample->az / 16384.0;
    const double synthetic_gx = sample->gx / 131.0;
    const double synthetic_gy = sample->gy / 131.0;
    const double synthetic_gz = sample->gz / 131.0;

    char json[UART_LINE_MAX];
    int written = snprintf(json,
                           sizeof(json),
                           "{\"node_id\":\"%s\",\"src_mac\":\"%s\",\"packet_id\":%u,\"batch_id\":%u,\"ts_gateway_ms\":%" PRIu32 ",\"node_tx_timestamp_ms\":%" PRIu32 ",\"latency_ms\":%" PRIu32 ",\"rssi\":%d,\"lqi\":%d,\"duplicate\":%s,\"decrypt_ok\":%s,\"values\":{\"synthetic_ax\":%.5f,\"synthetic_ay\":%.5f,\"synthetic_az\":%.5f,\"synthetic_gx\":%.5f,\"synthetic_gy\":%.5f,\"synthetic_gz\":%.5f}}",
                           received_packet->node_id,
                           received_packet->src_mac,
                           (unsigned)packet->batch_id,
                           (unsigned)packet->batch_id,
                           ts_gateway_ms,
                           node_tx_timestamp_ms,
                           latency_ms,
                           received_packet->rssi,
                           received_packet->lqi,
                           received_packet->duplicate ? "true" : "false",
                           received_packet->decrypt_ok ? "true" : "false",
                           synthetic_ax,
                           synthetic_ay,
                           synthetic_az,
                           synthetic_gx,
                           synthetic_gy,
                           synthetic_gz);

    if (written <= 0 || written >= (int)sizeof(json)) {
        ESP_LOGE(TAG, "JSON demasiado largo o error al generarlo");
        return;
    }

    ESP_LOGI(TAG, "Paquete ESPNOW valido recibido. batch_id=%u", (unsigned)packet->batch_id);
    ESP_LOGI(TAG, "JSON generado: %s", json);

    uart_write_bytes(UART_PORT_NUM, json, written);
    uart_write_bytes(UART_PORT_NUM, "\n", 1);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Gateway estrella ESP-NOW -> UART iniciado");

    ESP_ERROR_CHECK(init_uart_to_mqtt_gateway());
    ESP_ERROR_CHECK(wifi_init_sta_radio_only());

    s_packet_queue = xQueueCreate(20, sizeof(received_packet_t));
    if (s_packet_queue == NULL) {
        ESP_LOGE(TAG, "No se pudo crear la cola de paquetes");
        return;
    }

    ESP_ERROR_CHECK(espnow_init_secure_star());

    while (true) {
        received_packet_t received_packet;
        if (xQueueReceive(s_packet_queue, &received_packet, portMAX_DELAY) == pdTRUE) {
            build_and_send_required_json(&received_packet);
        }
    }
}
