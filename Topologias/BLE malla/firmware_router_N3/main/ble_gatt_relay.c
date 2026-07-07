#include "ble_gatt_relay.h"
#include "config.h"
#include "telemetry_packet.h"
#include "crypto_helper.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "esp_bt.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "BLE_GATT_RELAY_N3";

#define RELAY_PAYLOAD_LEN       (5 + sizeof(sensor_packet_t) + AES_GCM_TAG_LEN)
#define RELAY_QUEUE_LEN         12
#define RELAY_OWN_RSSI          0

/*
 * N3 conserva dos funciones:
 *  1) Central BLE temporal frente a N1/N2 para leer sus paquetes seguros.
 *  2) Periferico BLE persistente frente al Gateway para entregar la cola por Notify.
 *
 * Asi el salto N3->Gateway ya no repite scan/connect/security/discovery/read por cada paquete.
 */
#define RELAY_ENABLE_OWN_TELEMETRY 1
#define RELAY_OWN_TASK_STACK       4096
#define RELAY_OWN_TASK_PRIORITY    4

static const ble_uuid128_t s_telemetry_svc_uuid =
    BLE_UUID128_INIT(0x01, 0x1c, 0xf4, 0xea, 0xf9, 0x39, 0xa1, 0x86,
                     0x4e, 0x4f, 0x91, 0x6f, 0x01, 0x00, 0x2a, 0x9f);
static const ble_uuid128_t s_telemetry_chr_uuid =
    BLE_UUID128_INIT(0x01, 0x1c, 0xf4, 0xea, 0xf9, 0x39, 0xa1, 0x86,
                     0x4e, 0x4f, 0x91, 0x6f, 0x02, 0x00, 0x2a, 0x9f);

typedef struct {
    uint8_t data[RELAY_PAYLOAD_LEN];
    size_t len;
    int8_t child_rssi;
} relay_payload_t;

typedef enum {
    RELAY_ROLE_NONE = 0,
    RELAY_ROLE_CHILD,
    RELAY_ROLE_GATEWAY,
} relay_role_t;

static relay_payload_t s_queue[RELAY_QUEUE_LEN];
static uint8_t s_queue_head;
static uint8_t s_queue_tail;
static uint8_t s_queue_count;

static uint8_t s_own_addr_type;
static uint16_t s_child_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_gateway_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_relay_chr_val_handle;

static bool s_child_connecting;
static bool s_gateway_advertising;
static bool s_gateway_subscribed;
static int8_t s_current_child_rssi;
static uint16_t s_own_batch_id;
static relay_role_t s_pending_role = RELAY_ROLE_NONE;

static uint16_t s_child_svc_start_handle;
static uint16_t s_child_svc_end_handle;
static uint16_t s_child_chr_val_handle;

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg);
static void start_child_scan(void);
static void start_gateway_advertising(void);
static void notify_gateway_from_queue(void);
static bool queue_push(const uint8_t *data, size_t len, int8_t child_rssi);
static esp_err_t enqueue_own_telemetry_packet(void);
static void own_telemetry_task(void *param);

static bool queue_has_payload(void)
{
    return s_queue_count > 0;
}

static bool queue_peek(relay_payload_t *out)
{
    if (out == NULL || s_queue_count == 0) {
        return false;
    }
    *out = s_queue[s_queue_head];
    return true;
}

static void queue_pop(void)
{
    if (s_queue_count == 0) {
        return;
    }
    memset(&s_queue[s_queue_head], 0, sizeof(s_queue[s_queue_head]));
    s_queue_head = (uint8_t)((s_queue_head + 1U) % RELAY_QUEUE_LEN);
    s_queue_count--;
}

static bool queue_push(const uint8_t *data, size_t len, int8_t child_rssi)
{
    if (data == NULL || len != RELAY_PAYLOAD_LEN) {
        return false;
    }

    if (s_queue_count >= RELAY_QUEUE_LEN) {
        ESP_LOGW(TAG, "Cola relay llena; payload descartado");
        return false;
    }

    relay_payload_t *slot = &s_queue[s_queue_tail];
    memcpy(slot->data, data, len);
    slot->len = len;
    slot->child_rssi = child_rssi;

    s_queue_tail = (uint8_t)((s_queue_tail + 1U) % RELAY_QUEUE_LEN);
    s_queue_count++;

    notify_gateway_from_queue();
    return true;
}

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

static int16_t synthetic_rand_i16(int16_t min_value, int16_t max_value)
{
    uint32_t span = (uint32_t)((int32_t)max_value - (int32_t)min_value + 1);
    return (int16_t)((int32_t)min_value + (int32_t)(esp_random() % span));
}

static esp_err_t fill_own_synthetic_packet(sensor_packet_t *packet)
{
    if (packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(packet, 0, sizeof(*packet));
    strlcpy(packet->node_id, NODE_ID, sizeof(packet->node_id));
    fill_src_mac(packet->src_mac);
    packet->start_timestamp_ms = get_timestamp_ms();
    packet->batch_id = s_own_batch_id;

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
             "N3 sintetico raw[0]: ax=%d ay=%d az=%d gx=%d gy=%d gz=%d",
             packet->muestras[0].ax,
             packet->muestras[0].ay,
             packet->muestras[0].az,
             packet->muestras[0].gx,
             packet->muestras[0].gy,
             packet->muestras[0].gz);
#endif

    return ESP_OK;
}

static esp_err_t enqueue_own_telemetry_packet(void)
{
    sensor_packet_t packet;
    esp_err_t err = fill_own_synthetic_packet(&packet);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "N3 no pudo generar paquete propio: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t encrypted_payload[sizeof(sensor_packet_t)];
    uint8_t auth_tag[AES_GCM_TAG_LEN];
    uint8_t gatt_payload[RELAY_PAYLOAD_LEN];
    memset(encrypted_payload, 0, sizeof(encrypted_payload));
    memset(auth_tag, 0, sizeof(auth_tag));
    memset(gatt_payload, 0, sizeof(gatt_payload));

    err = crypto_encrypt_payload((const uint8_t *)&packet,
                                 sizeof(packet),
                                 packet.batch_id,
                                 NODE_CRYPTO_ID,
                                 encrypted_payload,
                                 auth_tag);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "N3 error cifrando paquete propio: %s", esp_err_to_name(err));
        return err;
    }

    size_t idx = 0;
    gatt_payload[idx++] = (uint8_t)(BLE_COMPANY_ID & 0xFF);
    gatt_payload[idx++] = (uint8_t)((BLE_COMPANY_ID >> 8) & 0xFF);
    gatt_payload[idx++] = (uint8_t)(packet.batch_id & 0xFF);
    gatt_payload[idx++] = (uint8_t)((packet.batch_id >> 8) & 0xFF);
    gatt_payload[idx++] = (uint8_t)NODE_CRYPTO_ID;
    memcpy(&gatt_payload[idx], encrypted_payload, sizeof(encrypted_payload));
    idx += sizeof(encrypted_payload);
    memcpy(&gatt_payload[idx], auth_tag, AES_GCM_TAG_LEN);
    idx += AES_GCM_TAG_LEN;

    if (idx != RELAY_PAYLOAD_LEN) {
        ESP_LOGE(TAG, "N3 longitud paquete propio invalida: %u esperado=%u",
                 (unsigned)idx, (unsigned)RELAY_PAYLOAD_LEN);
        return ESP_FAIL;
    }

    if (!queue_push(gatt_payload, idx, RELAY_OWN_RSSI)) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "N3 encolo paquete propio AES-GCM node_id=%s batch_id=%u bytes=%u. Cola=%u",
             packet.node_id,
             packet.batch_id,
             (unsigned)idx,
             (unsigned)s_queue_count);
    s_own_batch_id++;

    return ESP_OK;
}

static void own_telemetry_task(void *param)
{
    (void)param;

#if RELAY_ENABLE_OWN_TELEMETRY
    ESP_LOGI(TAG,
             "N3 generara telemetria propia cada %d s, marcada como NODE_CRYPTO_ID=%u",
             NODE_WAKEUP_PERIOD_SEC,
             (unsigned)NODE_CRYPTO_ID);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)NODE_WAKEUP_PERIOD_SEC * 1000U));
        (void)enqueue_own_telemetry_packet();
    }
#else
    vTaskDelete(NULL);
#endif
}

static int relay_telemetry_access_cb(uint16_t conn_handle,
                                     uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt,
                                     void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    relay_payload_t payload;
    if (!queue_peek(&payload)) {
        ESP_LOGW(TAG, "Gateway leyo N3, pero no hay payload pendiente");
        return BLE_ATT_ERR_UNLIKELY;
    }

    int rc = os_mbuf_append(ctxt->om, payload.data, payload.len);
    if (rc != 0) {
        ESP_LOGE(TAG, "No se pudo copiar payload relay a GATT rc=%d", rc);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    ESP_LOGI(TAG,
             "Payload cifrado entregado por READ seguro. conn=%u bytes=%u child_rssi=%d cola_restante=%u",
             conn_handle,
             (unsigned)payload.len,
             (int)payload.child_rssi,
             (unsigned)(s_queue_count - 1U));

    queue_pop();
    notify_gateway_from_queue();
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_telemetry_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_telemetry_chr_uuid.u,
                .access_cb = relay_telemetry_access_cb,
                .val_handle = &s_relay_chr_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 }
};

static void notify_gateway_from_queue(void)
{
    if (s_gateway_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_gateway_subscribed) {
        return;
    }

    while (queue_has_payload()) {
        relay_payload_t payload;
        if (!queue_peek(&payload)) {
            return;
        }

        struct os_mbuf *om = ble_hs_mbuf_from_flat(payload.data, payload.len);
        if (om == NULL) {
            ESP_LOGW(TAG, "No hay mbuf para notify; se reintentara luego");
            return;
        }

        int rc = ble_gatts_notify_custom(s_gateway_conn_handle, s_relay_chr_val_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_gatts_notify_custom fallo rc=%d; payload queda en cola", rc);
            return;
        }

        ESP_LOGI(TAG,
                 "Notify seguro N3->Gateway enviado. conn=%u bytes=%u child_rssi=%d cola_restante=%u",
                 s_gateway_conn_handle,
                 (unsigned)payload.len,
                 (int)payload.child_rssi,
                 (unsigned)(s_queue_count - 1U));
        queue_pop();
    }
}

static void set_tx_power_p3(void)
{
    esp_err_t ret = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P6);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo configurar BLE TX DEFAULT: %s", esp_err_to_name(ret));
    }

    ret = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P6);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo configurar BLE TX ADV: %s", esp_err_to_name(ret));
    }

    ret = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, ESP_PWR_LVL_P6);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo configurar BLE TX SCAN: %s", esp_err_to_name(ret));
    }
}

static void stop_gateway_advertising(void)
{
    if (!s_gateway_advertising) {
        return;
    }

    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_adv_stop aviso rc=%d", rc);
    }
    s_gateway_advertising = false;
}

static void start_gateway_advertising(void)
{
    if (s_gateway_conn_handle != BLE_HS_CONN_HANDLE_NONE || s_gateway_advertising) {
        return;
    }

    const size_t name_len = strlen(BLE_RELAY_DEVICE_NAME);
    if ((name_len + 5U) > 31U || (name_len + 2U) > 31U) {
        ESP_LOGE(TAG, "Nombre BLE N3 demasiado largo: %u", (unsigned)name_len);
        return;
    }

    uint8_t adv_data[31];
    size_t adv_idx = 0;
    adv_data[adv_idx++] = 0x02;
    adv_data[adv_idx++] = 0x01;
    adv_data[adv_idx++] = (BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    adv_data[adv_idx++] = (uint8_t)(name_len + 1U);
    adv_data[adv_idx++] = 0x09;
    memcpy(&adv_data[adv_idx], BLE_RELAY_DEVICE_NAME, name_len);
    adv_idx += name_len;

    int rc = ble_gap_adv_set_data(adv_data, (int)adv_idx);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_data N3 fallo rc=%d", rc);
        return;
    }

    uint8_t scan_rsp[31];
    memset(scan_rsp, 0, sizeof(scan_rsp));
    scan_rsp[0] = (uint8_t)(name_len + 1U);
    scan_rsp[1] = 0x09;
    memcpy(&scan_rsp[2], BLE_RELAY_DEVICE_NAME, name_len);

    rc = ble_gap_adv_rsp_set_data(scan_rsp, (int)(name_len + 2U));
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_data N3 fallo rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = 0x0020;
    adv_params.itvl_max = 0x0020;
    adv_params.channel_map = 0x07;
    adv_params.filter_policy = 0;

    int rc_start = ble_gap_adv_start(s_own_addr_type,
                                     NULL,
                                     BLE_HS_FOREVER,
                                     &adv_params,
                                     ble_gap_event_cb,
                                     NULL);
    if (rc_start != 0 && rc_start != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start N3 fallo rc=%d", rc_start);
    } else {
        s_gateway_advertising = true;
        ESP_LOGI(TAG,
                 "N3 anunciando para conexion persistente del Gateway (%s). Cola=%u",
                 BLE_RELAY_DEVICE_NAME,
                 (unsigned)s_queue_count);
    }
}

static bool adv_name_matches_child(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        return false;
    }

    if (fields.name == NULL) {
        return false;
    }

    if ((fields.name_len == strlen(BLE_CHILD_NAME_N1)) &&
        (memcmp(fields.name, BLE_CHILD_NAME_N1, fields.name_len) == 0)) {
        return true;
    }

    if ((fields.name_len == strlen(BLE_CHILD_NAME_N2)) &&
        (memcmp(fields.name, BLE_CHILD_NAME_N2, fields.name_len) == 0)) {
        return true;
    }

    return false;
}

static void connect_to_child(const struct ble_gap_disc_desc *disc)
{
    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc_cancel aviso rc=%d", rc);
    }

    s_current_child_rssi = disc->rssi;
    s_child_connecting = true;
    s_pending_role = RELAY_ROLE_CHILD;

    ESP_LOGI(TAG, "Nodo hijo N1/N2 encontrado RSSI=%d; N3 conectando con GATT seguro", (int)s_current_child_rssi);
    rc = ble_gap_connect(s_own_addr_type,
                         &disc->addr,
                         3000,
                         NULL,
                         ble_gap_event_cb,
                         NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect hijo fallo rc=%d", rc);
        s_child_connecting = false;
        s_pending_role = RELAY_ROLE_NONE;
        start_child_scan();
    }
}

static bool relay_validate_payload(const uint8_t *data, size_t len)
{
    if (data == NULL || len != RELAY_PAYLOAD_LEN) {
        ESP_LOGE(TAG, "Payload hijo invalido: recibido=%u esperado=%u",
                 (unsigned)len, (unsigned)RELAY_PAYLOAD_LEN);
        return false;
    }

    const uint16_t company_id = data[0] | ((uint16_t)data[1] << 8);
    const uint8_t node_crypto_id = data[4];

    if (company_id != BLE_COMPANY_ID) {
        ESP_LOGW(TAG, "Company ID invalido en relay: 0x%04X", company_id);
        return false;
    }

    if (node_crypto_id != 1U && node_crypto_id != 2U) {
        ESP_LOGW(TAG, "N3 solo reenvia N1/N2. NODE_CRYPTO_ID recibido=%u", (unsigned)node_crypto_id);
        return false;
    }

    return true;
}

static int child_read_chr_cb(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr,
                             void *arg)
{
    (void)arg;

    if (error->status != 0) {
        ESP_LOGE(TAG, "N3 lectura GATT hijo fallo status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    uint16_t om_len = OS_MBUF_PKTLEN(attr->om);
    uint8_t buffer[RELAY_PAYLOAD_LEN];

    if (om_len > sizeof(buffer)) {
        ESP_LOGE(TAG, "Payload hijo demasiado grande: %u", om_len);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    int rc = os_mbuf_copydata(attr->om, 0, om_len, buffer);
    if (rc != 0) {
        ESP_LOGE(TAG, "os_mbuf_copydata hijo fallo rc=%d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    if (relay_validate_payload(buffer, om_len)) {
        if (queue_push(buffer, om_len, s_current_child_rssi)) {
            const uint16_t batch_id = buffer[2] | ((uint16_t)buffer[3] << 8);
            const uint8_t node_crypto_id = buffer[4];
            ESP_LOGI(TAG,
                     "N3 guardo payload cifrado de N%u batch_id=%u bytes=%u. Cola=%u",
                     (unsigned)node_crypto_id,
                     batch_id,
                     (unsigned)om_len,
                     (unsigned)s_queue_count);
        }
    }

    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static void read_child_secure_telemetry(uint16_t conn_handle)
{
    if (s_child_chr_val_handle == 0) {
        ESP_LOGE(TAG, "No hay handle valido para lectura GATT del hijo");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    ESP_LOGI(TAG, "N3 leyendo caracteristica segura del hijo. val_handle=%u", s_child_chr_val_handle);
    int rc = ble_gattc_read(conn_handle, s_child_chr_val_handle, child_read_chr_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_read hijo fallo rc=%d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static int child_chr_disc_cb(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             const struct ble_gatt_chr *chr,
                             void *arg)
{
    (void)arg;

    if (error->status == 0 && chr != NULL) {
        s_child_chr_val_handle = chr->val_handle;
        ESP_LOGI(TAG, "Caracteristica hija encontrada. val_handle=%u", s_child_chr_val_handle);
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (s_child_chr_val_handle == 0) {
            ESP_LOGE(TAG, "No se encontro caracteristica de telemetria en hijo");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }

        ESP_LOGI(TAG, "Descubrimiento de caracteristica hija completado; leyendo payload cifrado");
        read_child_secure_telemetry(conn_handle);
        return 0;
    }

    ESP_LOGE(TAG, "Descubrimiento caracteristica hija fallo status=%d", error->status);
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static int child_svc_disc_cb(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             const struct ble_gatt_svc *service,
                             void *arg)
{
    (void)arg;

    if (error->status == 0 && service != NULL) {
        s_child_svc_start_handle = service->start_handle;
        s_child_svc_end_handle = service->end_handle;
        s_child_chr_val_handle = 0;
        ESP_LOGI(TAG, "Servicio hijo encontrado. handles=%u-%u",
                 s_child_svc_start_handle, s_child_svc_end_handle);

        int rc = ble_gattc_disc_chrs_by_uuid(conn_handle,
                                             s_child_svc_start_handle,
                                             s_child_svc_end_handle,
                                             &s_telemetry_chr_uuid.u,
                                             child_chr_disc_cb,
                                             NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_gattc_disc_chrs_by_uuid hijo fallo rc=%d", rc);
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (s_child_svc_start_handle == 0) {
            ESP_LOGE(TAG, "No se encontro servicio de telemetria en hijo");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }

    ESP_LOGE(TAG, "Descubrimiento servicio hijo fallo status=%d", error->status);
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static void discover_child_secure_telemetry(uint16_t conn_handle)
{
    s_child_svc_start_handle = 0;
    s_child_svc_end_handle = 0;
    s_child_chr_val_handle = 0;

    ESP_LOGI(TAG, "N3 descubriendo servicio GATT seguro del hijo");
    int rc = ble_gattc_disc_svc_by_uuid(conn_handle,
                                        &s_telemetry_svc_uuid.u,
                                        child_svc_disc_cb,
                                        NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_disc_svc_by_uuid hijo fallo rc=%d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static int child_mtu_exchange_cb(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 uint16_t mtu,
                                 void *arg)
{
    (void)arg;

    if (error->status == 0) {
        ESP_LOGI(TAG, "MTU GATT hijo negociado=%u", mtu);
    } else {
        ESP_LOGW(TAG, "MTU exchange hijo fallo status=%d; se intentara leer de todas formas", error->status);
    }

    discover_child_secure_telemetry(conn_handle);
    return 0;
}

static void exchange_child_mtu_then_discover(uint16_t conn_handle)
{
    int rc = ble_gattc_exchange_mtu(conn_handle, child_mtu_exchange_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gattc_exchange_mtu hijo fallo rc=%d; descubriendo sin exchange", rc);
        discover_child_secure_telemetry(conn_handle);
    }
}

static void start_child_scan(void)
{
    if (s_child_conn_handle != BLE_HS_CONN_HANDLE_NONE || s_child_connecting) {
        return;
    }

    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto fallo: %d", rc);
        return;
    }

    struct ble_gap_disc_params params;
    memset(&params, 0, sizeof(params));
    params.filter_duplicates = 0;
    params.passive = 0;
    params.itvl = BLE_SCAN_INTERVAL;
    params.window = BLE_SCAN_WINDOW;
    params.filter_policy = 0;
    params.limited = 0;

    rc = ble_gap_disc(s_own_addr_type,
                      BLE_HS_FOREVER,
                      &params,
                      ble_gap_event_cb,
                      NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc hijo fallo: %d", rc);
    } else {
        ESP_LOGI(TAG,
                 "N3 escaneando N1/N2. Gateway_conn=%s interval=0x%04X window=0x%04X",
                 (s_gateway_conn_handle != BLE_HS_CONN_HANDLE_NONE) ? "si" : "no",
                 BLE_SCAN_INTERVAL,
                 BLE_SCAN_WINDOW);
    }
}

static void handle_connection_established(uint16_t conn_handle, relay_role_t role)
{
    if (role == RELAY_ROLE_CHILD) {
        s_child_conn_handle = conn_handle;
        s_child_connecting = false;
        ESP_LOGI(TAG, "Conexion BLE N1/N2->N3 establecida. conn_handle=%u", conn_handle);
    } else {
        s_gateway_conn_handle = conn_handle;
        s_gateway_advertising = false;
        s_gateway_subscribed = false;
        ESP_LOGI(TAG, "Conexion BLE persistente N3->Gateway establecida. conn_handle=%u", conn_handle);
    }

    ESP_LOGI(TAG, "Iniciando seguridad BLE nativa: pairing + bonding + cifrado");
    int rc = ble_gap_security_initiate(conn_handle);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_security_initiate fallo rc=%d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    } else if (rc == BLE_HS_EALREADY && role == RELAY_ROLE_CHILD) {
        exchange_child_mtu_then_discover(conn_handle);
    } else if (rc == BLE_HS_EALREADY && role == RELAY_ROLE_GATEWAY) {
        notify_gateway_from_queue();
    }
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (!s_child_connecting && s_child_conn_handle == BLE_HS_CONN_HANDLE_NONE &&
            adv_name_matches_child(&event->disc)) {
            connect_to_child(&event->disc);
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT: {
        relay_role_t role = (s_pending_role == RELAY_ROLE_CHILD) ? RELAY_ROLE_CHILD : RELAY_ROLE_GATEWAY;
        s_pending_role = RELAY_ROLE_NONE;

        if (event->connect.status == 0) {
            handle_connection_established(event->connect.conn_handle, role);
        } else {
            ESP_LOGW(TAG, "Conexion BLE fallida status=%d role=%s",
                     event->connect.status,
                     (role == RELAY_ROLE_CHILD) ? "N1/N2->N3" : "N3->Gateway");
            if (role == RELAY_ROLE_CHILD) {
                s_child_connecting = false;
                s_child_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                start_child_scan();
            } else {
                s_gateway_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                s_gateway_subscribed = false;
                start_gateway_advertising();
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            if (event->enc_change.conn_handle == s_child_conn_handle) {
                ESP_LOGI(TAG, "Enlace BLE cifrado N1/N2->N3; leyendo hijo");
                exchange_child_mtu_then_discover(event->enc_change.conn_handle);
            } else if (event->enc_change.conn_handle == s_gateway_conn_handle) {
                ESP_LOGI(TAG, "Enlace BLE cifrado persistente N3->Gateway listo; esperando subscription Notify");
                notify_gateway_from_queue();
            }
        } else {
            ESP_LOGE(TAG, "Fallo cifrado BLE status=%d", event->enc_change.status);
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t disc_handle = event->disconnect.conn.conn_handle;
        ESP_LOGI(TAG, "BLE desconectado reason=%d conn=%u", event->disconnect.reason, disc_handle);

        if (disc_handle == s_child_conn_handle) {
            s_child_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_child_connecting = false;
            s_child_svc_start_handle = 0;
            s_child_svc_end_handle = 0;
            s_child_chr_val_handle = 0;
            start_child_scan();
        } else if (disc_handle == s_gateway_conn_handle) {
            s_gateway_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_gateway_subscribed = false;
            s_gateway_advertising = false;
            start_gateway_advertising();
            start_child_scan();
        } else {
            s_child_connecting = false;
            start_child_scan();
        }
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_relay_chr_val_handle &&
            event->subscribe.conn_handle == s_gateway_conn_handle) {
            s_gateway_subscribed = event->subscribe.cur_notify ? true : false;
            ESP_LOGI(TAG,
                     "Gateway %s a Notify de N3. conn=%u attr=%u cola=%u",
                     s_gateway_subscribed ? "suscrito" : "desuscrito",
                     event->subscribe.conn_handle,
                     event->subscribe.attr_handle,
                     (unsigned)s_queue_count);
            notify_gateway_from_queue();
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        ESP_LOGW(TAG, "Repeat pairing detectado; se permite actualizar bonding");
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_gateway_advertising = false;
        ESP_LOGI(TAG, "Advertising N3 completado; reiniciando si Gateway no esta conectado");
        start_gateway_advertising();
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "Escaneo hijo completado; reiniciando");
        start_child_scan();
        return 0;

    default:
        return 0;
    }
}

static void ble_app_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto fallo rc=%d", rc);
        return;
    }

    set_tx_power_p3();
    ESP_LOGI(TAG,
             "NimBLE sincronizado. N3: Gateway persistente por Notify + escaneo N1/N2. TX_PWR=P6, STREAM_SAMPLE_COUNT=%d",
             STREAM_SAMPLE_COUNT);

    start_gateway_advertising();
    start_child_scan();
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_gatt_relay_init(void)
{
    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init fallo: %d", rc);
        return ESP_FAIL;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(BLE_RELAY_DEVICE_NAME);

    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg fallo rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs fallo rc=%d", rc);
        return ESP_FAIL;
    }

    ble_hs_cfg.sync_cb = ble_app_on_sync;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    nimble_port_freertos_init(nimble_host_task);

#if RELAY_ENABLE_OWN_TELEMETRY
    BaseType_t task_ok = xTaskCreate(own_telemetry_task,
                                     "n3_own_telemetry",
                                     RELAY_OWN_TASK_STACK,
                                     NULL,
                                     RELAY_OWN_TASK_PRIORITY,
                                     NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "No se pudo crear tarea de telemetria propia N3");
        return ESP_FAIL;
    }
#endif

    return ESP_OK;
}
