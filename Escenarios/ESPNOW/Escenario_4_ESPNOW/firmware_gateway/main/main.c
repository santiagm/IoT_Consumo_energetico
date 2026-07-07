#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <time.h>

#include "config.h"
#include "security_payload.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/portmacro.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"

#include "mqtt_client.h"

#include "lwip/apps/sntp.h"
#include "sys/time.h"
#include "cJSON.h"

#include "psa/crypto.h"

#define WIFI_CONNECTED_BIT      BIT0

static const char *TAG = "GATEWAY_ESC4_SEC";

/*
 * ============================================================
 * ESCENARIO 4 - GATEWAY
 * ============================================================
 *
 * Gateway para ESP-NOW + seguridad PMK/LMK + AES-128-GCM de aplicacion.
 * El nodo usa MPU6050 fisico, cifra sensor_packet_t completo con AES-GCM
 * y el gateway solo publica si el tag GCM es autentico.
 */

static const uint8_t NODE_PEER_MAC[ESP_NOW_ETH_ALEN] = NODE_PEER_MAC_BYTES;
static const uint8_t ESPNOW_PMK[16] = ESPNOW_PMK_BYTES;
static const uint8_t ESPNOW_LMK[16] = ESPNOW_LMK_BYTES;

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
    char src_mac[18];
    sensor_packet_t packet;
} received_packet_t;

_Static_assert(sizeof(sensor_packet_t) <= ESP_NOW_MAX_DATA_LEN,
               "sensor_packet_t exceeds ESP-NOW payload limit. Reduce STREAM_SAMPLE_COUNT");

_Static_assert(sizeof(secure_espnow_packet_t) <= ESP_NOW_MAX_DATA_LEN,
               "secure_espnow_packet_t exceeds ESP-NOW payload limit. Reduce STREAM_SAMPLE_COUNT");

static const uint8_t s_app_aes_key[16] = APP_AES_GCM_KEY_BYTES;

static EventGroupHandle_t s_wifi_event_group;
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_connected = false;
static QueueHandle_t s_packet_queue = NULL;
static bool s_sntp_started = false;
static bool s_last_batch_valid = false;
static uint16_t s_last_batch_id = 0;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data);

static void mac_to_string(const uint8_t *mac, char *mac_str, size_t mac_str_len)
{
    snprintf(mac_str, mac_str_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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

static bool is_duplicate_batch(uint16_t batch_id)
{
    if (s_last_batch_valid && batch_id == s_last_batch_id) {
        return true;
    }

    s_last_batch_id = batch_id;
    s_last_batch_valid = true;
    return false;
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

    ESP_LOGI(TAG,
             "AES-128-GCM valido batch=%u payload_len=%u",
             (unsigned)plain_packet->batch_id,
             (unsigned)secure_packet->encrypted_len);

    return ESP_OK;
}

static int64_t get_unix_time_ms(void)
{
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000LL + (int64_t)(tv.tv_usec / 1000);
}

static void start_sntp_client(void)
{
    if (s_sntp_started) {
        return;
    }

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, SNTP_SERVER);
    sntp_init();

    s_sntp_started = true;
    ESP_LOGI(TAG, "Cliente SNTP iniciado con servidor %s", SNTP_SERVER);
}

static void wait_for_sntp_sync(uint32_t timeout_ms)
{
    const TickType_t poll_delay = pdMS_TO_TICKS(500);
    TickType_t elapsed = 0;
    const time_t valid_epoch_threshold = (time_t)1700000000;

    while (elapsed < pdMS_TO_TICKS(timeout_ms)) {
        if (time(NULL) >= valid_epoch_threshold) {
            ESP_LOGI(TAG, "Hora NTP valida disponible");
            break;
        }

        vTaskDelay(poll_delay);
        elapsed += poll_delay;
    }

    if (time(NULL) < valid_epoch_threshold) {
        ESP_LOGW(TAG, "SNTP aun no sincronizado, se usara la hora disponible del sistema");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA iniciado, intentando conectar...");
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi desconectado, intentando reconectar...");
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip_str[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "WiFi conectado, IP=%s", ip_str);

        start_sntp_client();

        uint8_t primary = 0;
        wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;

        if (esp_wifi_get_channel(&primary, &secondary) == ESP_OK) {
            ESP_LOGI(TAG, "Canal WiFi actual: %u", primary);

            if (primary != ESPNOW_CHANNEL) {
                ESP_LOGW(TAG,
                         "ADVERTENCIA: WiFi esta en canal %u y ESPNOW_CHANNEL=%d. "
                         "El AP/hotspot debe estar en el mismo canal que el nodo.",
                         primary, ESPNOW_CHANNEL);
            } else {
                ESP_LOGI(TAG, "Canal WiFi y ESP-NOW coinciden correctamente");
            }
        }
    }
}

static esp_err_t wifi_init_sta(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           pdMS_TO_TICKS(20000));

    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "No se obtuvo IP en el tiempo de espera; se continua en modo asincrono");
    }

    wait_for_sntp_sync(10000);
    return ESP_OK;
}

static esp_mqtt_client_handle_t mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .uri = "mqtt://" THINGSBOARD_HOST ":" STR(THINGSBOARD_PORT)
            }
        },
        .credentials = {
            .client_id = MQTT_CLIENT_ID,
            .username = THINGSBOARD_TOKEN,
            .authentication = {
                .password = ""
            }
        }
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Error inicializando cliente MQTT");
        return NULL;
    }

    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    return client;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT conectado");
        s_mqtt_connected = true;
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT desconectado");
        s_mqtt_connected = false;
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "Error MQTT");
        s_mqtt_connected = false;
        break;

    default:
        break;
    }
}

static void send_json_to_thingsboard(const received_packet_t *received_packet)
{
    if (!s_mqtt_connected) {
        ESP_LOGW(TAG, "MQTT no conectado, paquete no publicado");
        return;
    }

    const sensor_packet_t *packet = &received_packet->packet;
    const int64_t base_timestamp_ms = get_unix_time_ms();

    cJSON *root = cJSON_CreateArray();
    if (root == NULL) {
        ESP_LOGE(TAG, "Error creando JSON");
        return;
    }

    for (int i = 0; i < STREAM_SAMPLE_COUNT; ++i) {
        int64_t timestamp = base_timestamp_ms + (int64_t)(i * SYNTHETIC_SAMPLE_DELAY_MS);

        mpu_sample_t sample;
        memcpy(&sample, &packet->muestras[i], sizeof(sample));

        double accel_x_g = sample.ax / 16384.0;
        double accel_y_g = sample.ay / 16384.0;
        double accel_z_g = sample.az / 16384.0;
        double gyro_x_dps = sample.gx / 131.0;
        double gyro_y_dps = sample.gy / 131.0;
        double gyro_z_dps = sample.gz / 131.0;

        cJSON *entry = cJSON_CreateObject();
        cJSON *values = cJSON_CreateObject();

        if (entry == NULL || values == NULL) {
            cJSON_Delete(entry);
            cJSON_Delete(values);
            cJSON_Delete(root);
            ESP_LOGE(TAG, "Error creando JSON de muestra");
            return;
        }

        cJSON_AddNumberToObject(entry, "ts", (double)timestamp);

        cJSON_AddStringToObject(values, "src_mac", received_packet->src_mac);
        cJSON_AddNumberToObject(values, "node_start_timestamp_ms", (double)packet->start_timestamp_ms);
        cJSON_AddNumberToObject(values, "batch_id", packet->batch_id);
        cJSON_AddNumberToObject(values, "accel_x_g", accel_x_g);
        cJSON_AddNumberToObject(values, "accel_y_g", accel_y_g);
        cJSON_AddNumberToObject(values, "accel_z_g", accel_z_g);
        cJSON_AddNumberToObject(values, "gyro_x_dps", gyro_x_dps);
        cJSON_AddNumberToObject(values, "gyro_y_dps", gyro_y_dps);
        cJSON_AddNumberToObject(values, "gyro_z_dps", gyro_z_dps);

        cJSON_AddItemToObject(entry, "values", values);
        cJSON_AddItemToArray(root, entry);
    }

    char *payload = cJSON_PrintUnformatted(root);
    if (payload == NULL) {
        ESP_LOGE(TAG, "Error serializando JSON");
        cJSON_Delete(root);
        return;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client,
                                         THINGSBOARD_TOPIC,
                                         payload,
                                         0,
                                         0,
                                         0);

    if (msg_id == -1) {
        ESP_LOGE(TAG, "Fallo al publicar en ThingsBoard");
    } else {
        ESP_LOGI(TAG, "Publicado mensaje MQTT id=%d", msg_id);
    }

    cJSON_free(payload);
    cJSON_Delete(root);
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

    const secure_espnow_packet_t *secure_packet = (const secure_espnow_packet_t *)data;

    ESP_LOGI(TAG,
             "ESP-NOW seguro recibido len=%d magic=0x%08" PRIX32 " version=%u batch=%u enc_len=%u",
             len,
             secure_packet->magic,
             (unsigned)secure_packet->version,
             (unsigned)secure_packet->batch_id,
             (unsigned)secure_packet->encrypted_len);

    received_packet_t received_packet;
    memset(&received_packet, 0, sizeof(received_packet));

    esp_err_t err = decrypt_secure_packet_gcm(secure_packet, &received_packet.packet);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Paquete descartado por validacion AES-GCM: %s", esp_err_to_name(err));
        return;
    }

    mac_to_string(info->src_addr,
                  received_packet.src_mac,
                  sizeof(received_packet.src_mac));

    if (received_packet.packet.src_mac[0] != '\0') {
        strncpy(received_packet.src_mac,
                received_packet.packet.src_mac,
                sizeof(received_packet.src_mac));
        received_packet.src_mac[sizeof(received_packet.src_mac) - 1] = '\0';
    }

    if (is_duplicate_batch(received_packet.packet.batch_id)) {
        ESP_LOGW(TAG,
                 "Batch duplicado descartado batch=%u src=%s",
                 (unsigned)received_packet.packet.batch_id,
                 received_packet.src_mac);
        return;
    }

    ESP_LOGI(TAG,
             "Paquete validado de %s, batch=%u, node_ts=%" PRIu32,
             received_packet.src_mac,
             (unsigned)received_packet.packet.batch_id,
             received_packet.packet.start_timestamp_ms);

    BaseType_t higher_woken = pdFALSE;

    if (xQueueSendFromISR(s_packet_queue,
                          &received_packet,
                          &higher_woken) != pdTRUE) {
        ESP_EARLY_LOGW(TAG, "Cola llena, paquete descartado");
    }

    if (higher_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t espnow_add_secure_peer(const uint8_t *peer_mac)
{
    esp_now_peer_info_t peer_info;
    memset(&peer_info, 0, sizeof(peer_info));

    memcpy(peer_info.peer_addr, peer_mac, ESP_NOW_ETH_ALEN);
    memcpy(peer_info.lmk, ESPNOW_LMK, sizeof(peer_info.lmk));
    peer_info.channel = ESPNOW_CHANNEL;
    peer_info.ifidx = WIFI_IF_STA;
    peer_info.encrypt = true;

    esp_err_t err;
    if (esp_now_is_peer_exist(peer_mac)) {
        err = esp_now_mod_peer(&peer_info);
    } else {
        err = esp_now_add_peer(&peer_info);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error agregando/modificando peer cifrado: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "Peer ESP-NOW cifrado listo: %02X:%02X:%02X:%02X:%02X:%02X",
             peer_mac[0], peer_mac[1], peer_mac[2],
             peer_mac[3], peer_mac[4], peer_mac[5]);

    return ESP_OK;
}

static esp_err_t espnow_init(void)
{
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error esp_now_init: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_now_set_pmk(ESPNOW_PMK);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando PMK: %s", esp_err_to_name(err));
        return err;
    }

    err = espnow_add_secure_peer(NODE_PEER_MAC);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_now_register_recv_cb(espnow_recv_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error registrando callback ESP-NOW: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "ESP-NOW seguro listo en canal %d", ESPNOW_CHANNEL);
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(wifi_init_sta());

    s_mqtt_client = mqtt_app_start();
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "No se pudo iniciar MQTT");
    }

    s_packet_queue = xQueueCreate(20, sizeof(received_packet_t));
    if (s_packet_queue == NULL) {
        ESP_LOGE(TAG, "No se pudo crear la cola de paquetes");
    }

    esp_err_t espnow_err = espnow_init();
    if (espnow_err != ESP_OK) {
        ESP_LOGE(TAG, "Error inicializando ESP-NOW seguro");
    }

    while (true) {
        received_packet_t received_packet;

        if (s_packet_queue != NULL &&
            xQueueReceive(s_packet_queue,
                          &received_packet,
                          pdMS_TO_TICKS(100)) == pdTRUE) {
            ESP_LOGI(TAG, "Procesando paquete de %s", received_packet.src_mac);
            send_json_to_thingsboard(&received_packet);
        }
    }
}
