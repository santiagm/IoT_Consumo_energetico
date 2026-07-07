#include "ble_scanner.h"

#include "config.h"
#include "crypto_helper.h"
#include "telemetry_packet.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern QueueHandle_t s_packet_queue;

static const char *TAG = "BLE_SEC_CLIENT";

#define BLE_DUPLICATE_WINDOW_US 8000000LL

static const ble_uuid128_t s_telemetry_svc_uuid =
    BLE_UUID128_INIT(0x01, 0x1c, 0xf4, 0xea, 0xf9, 0x39, 0xa1, 0x86,
                     0x4e, 0x4f, 0x91, 0x6f, 0x01, 0x00, 0x2a, 0x9f);
static const ble_uuid128_t s_telemetry_chr_uuid =
    BLE_UUID128_INIT(0x01, 0x1c, 0xf4, 0xea, 0xf9, 0x39, 0xa1, 0x86,
                     0x4e, 0x4f, 0x91, 0x6f, 0x02, 0x00, 0x2a, 0x9f);

static uint8_t s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_start_handle;
static uint16_t s_svc_end_handle;
static uint16_t s_chr_val_handle;
static bool s_connecting = false;

static int8_t s_current_rssi = 127;

typedef struct {
    bool used;
    char src_mac[18];
    uint16_t last_batch_id;
    int64_t last_batch_time_us;
} duplicate_slot_t;

static duplicate_slot_t s_duplicate_slots[BLE_STAR_MAX_NODES];

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg);
static void ble_scanner_start(void);
static void read_secure_telemetry(uint16_t conn_handle);

static bool is_duplicate_packet(const char *src_mac, uint16_t batch_id)
{
    int64_t now_us = esp_timer_get_time();
    int free_slot = -1;

    for (int i = 0; i < BLE_STAR_MAX_NODES; i++) {
        if (!s_duplicate_slots[i].used) {
            if (free_slot < 0) {
                free_slot = i;
            }
            continue;
        }

        if (strncmp(s_duplicate_slots[i].src_mac, src_mac, sizeof(s_duplicate_slots[i].src_mac)) == 0) {
            if ((batch_id == s_duplicate_slots[i].last_batch_id) &&
                ((now_us - s_duplicate_slots[i].last_batch_time_us) < BLE_DUPLICATE_WINDOW_US)) {
                return true;
            }

            s_duplicate_slots[i].last_batch_id = batch_id;
            s_duplicate_slots[i].last_batch_time_us = now_us;
            return false;
        }
    }

    int slot = (free_slot >= 0) ? free_slot : 0;
    memset(&s_duplicate_slots[slot], 0, sizeof(s_duplicate_slots[slot]));
    s_duplicate_slots[slot].used = true;
    strlcpy(s_duplicate_slots[slot].src_mac, src_mac, sizeof(s_duplicate_slots[slot].src_mac));
    s_duplicate_slots[slot].last_batch_id = batch_id;
    s_duplicate_slots[slot].last_batch_time_us = now_us;
    return false;
}

static bool decrypt_and_queue_packet(const uint8_t *data, size_t len)
{
    const size_t encrypted_len = sizeof(sensor_packet_t);
    const size_t expected_len = 5 + encrypted_len + AES_GCM_TAG_LEN;

    if (data == NULL || len != expected_len) {
        ESP_LOGE(TAG, "Tamano GATT invalido: recibido=%u esperado=%u",
                 (unsigned)len, (unsigned)expected_len);
        return false;
    }

    uint16_t company_id = data[0] | ((uint16_t)data[1] << 8);
    uint16_t batch_id = data[2] | ((uint16_t)data[3] << 8);
    uint8_t node_crypto_id = data[4];

    if (company_id != BLE_COMPANY_ID) {
        ESP_LOGW(TAG, "Company ID BLE invalido: 0x%04X", company_id);
        return false;
    }

    if (node_crypto_id < 1 || node_crypto_id > BLE_STAR_MAX_NODES) {
        ESP_LOGW(TAG, "NODE_CRYPTO_ID invalido: %u", (unsigned)node_crypto_id);
        return false;
    }

    const uint8_t *encrypted_data = &data[5];
    const uint8_t *auth_tag = &data[5 + encrypted_len];

    sensor_packet_t decrypted_packet;
    memset(&decrypted_packet, 0, sizeof(decrypted_packet));

    esp_err_t err = crypto_decrypt_payload(encrypted_data,
                                           encrypted_len,
                                           batch_id,
                                           node_crypto_id,
                                           auth_tag,
                                           (uint8_t *)&decrypted_packet);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error autenticacion AES-GCM; paquete descartado batch_id=%u", batch_id);
        return false;
    }

    if (decrypted_packet.batch_id != batch_id) {
        ESP_LOGW(TAG, "batch_id interno no coincide: gatt=%u payload=%u",
                 batch_id, decrypted_packet.batch_id);
        return false;
    }

    if (is_duplicate_packet(decrypted_packet.src_mac, decrypted_packet.batch_id)) {
        ESP_LOGI(TAG, "Duplicado BLE ignorado. node=%s mac=%s batch_id=%u",
                 decrypted_packet.node_id, decrypted_packet.src_mac, decrypted_packet.batch_id);
        return true;
    }

    gateway_packet_t rx_packet;
    memset(&rx_packet, 0, sizeof(rx_packet));
    rx_packet.packet = decrypted_packet;
    rx_packet.rssi = s_current_rssi;
    rx_packet.duplicate = false;
    rx_packet.decrypt_ok = true;

    if (xQueueSend(s_packet_queue, &rx_packet, 0) != pdPASS) {
        ESP_LOGW(TAG, "Cola llena, paquete BLE perdido");
        return false;
    }

    ESP_LOGI(TAG, "AES-GCM valido. Paquete BLE recibido node=%s batch_id=%u rssi=%d",
             decrypted_packet.node_id, decrypted_packet.batch_id, (int)s_current_rssi);
    return true;
}

static int read_chr_cb(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr,
                       void *arg)
{
    (void)arg;

    if (error->status != 0) {
        ESP_LOGE(TAG, "Lectura GATT fallo status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    uint16_t om_len = OS_MBUF_PKTLEN(attr->om);
    uint8_t buffer[5 + sizeof(sensor_packet_t) + AES_GCM_TAG_LEN];

    if (om_len > sizeof(buffer)) {
        ESP_LOGE(TAG, "Payload GATT demasiado grande: %u", om_len);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    int rc = os_mbuf_copydata(attr->om, 0, om_len, buffer);
    if (rc != 0) {
        ESP_LOGE(TAG, "os_mbuf_copydata fallo rc=%d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    decrypt_and_queue_packet(buffer, om_len);
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static void read_secure_telemetry(uint16_t conn_handle)
{
    if (s_chr_val_handle == 0) {
        ESP_LOGE(TAG, "No hay handle valido para lectura GATT");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    ESP_LOGI(TAG, "Leyendo caracteristica segura. val_handle=%u", s_chr_val_handle);
    int rc = ble_gattc_read(conn_handle, s_chr_val_handle, read_chr_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_read fallo rc=%d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static int chr_disc_cb(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr,
                       void *arg)
{
    (void)arg;

    if (error->status == 0 && chr != NULL) {
        s_chr_val_handle = chr->val_handle;
        ESP_LOGI(TAG, "Caracteristica segura encontrada. val_handle=%u", s_chr_val_handle);
        /*
         * No iniciar ble_gattc_read() aqui.
         * Este callback todavia pertenece al procedimiento de descubrimiento de caracteristicas.
         * Si se arranca la lectura antes de recibir BLE_HS_EDONE, se pueden solapar dos
         * procedimientos GATT y terminar en timeout/status=7. Primero esperamos EDONE.
         */
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (s_chr_val_handle == 0) {
            ESP_LOGE(TAG, "No se encontro caracteristica de telemetria");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }

        ESP_LOGI(TAG, "Descubrimiento de caracteristica completado; iniciando lectura GATT segura");
        read_secure_telemetry(conn_handle);
        return 0;
    }

    ESP_LOGE(TAG, "Descubrimiento caracteristica fallo status=%d", error->status);
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static int svc_disc_cb(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service,
                       void *arg)
{
    (void)arg;

    if (error->status == 0 && service != NULL) {
        s_svc_start_handle = service->start_handle;
        s_svc_end_handle = service->end_handle;
        s_chr_val_handle = 0;
        ESP_LOGI(TAG, "Servicio de telemetria encontrado. handles=%u-%u",
                 s_svc_start_handle, s_svc_end_handle);

        int rc = ble_gattc_disc_chrs_by_uuid(conn_handle,
                                             s_svc_start_handle,
                                             s_svc_end_handle,
                                             &s_telemetry_chr_uuid.u,
                                             chr_disc_cb,
                                             NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_gattc_disc_chrs_by_uuid fallo rc=%d", rc);
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (s_svc_start_handle == 0) {
            ESP_LOGE(TAG, "No se encontro servicio de telemetria");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }

    ESP_LOGE(TAG, "Descubrimiento servicio fallo status=%d", error->status);
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static void discover_secure_telemetry(uint16_t conn_handle)
{
    s_svc_start_handle = 0;
    s_svc_end_handle = 0;
    s_chr_val_handle = 0;

    ESP_LOGI(TAG, "Descubriendo servicio GATT de telemetria cifrada");
    int rc = ble_gattc_disc_svc_by_uuid(conn_handle,
                                        &s_telemetry_svc_uuid.u,
                                        svc_disc_cb,
                                        NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_disc_svc_by_uuid fallo rc=%d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}


static int mtu_exchange_cb(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           uint16_t mtu,
                           void *arg)
{
    (void)arg;

    if (error->status == 0) {
        ESP_LOGI(TAG, "MTU GATT negociado=%u", mtu);
    } else {
        ESP_LOGW(TAG, "MTU exchange fallo status=%d; se intentara leer de todas formas", error->status);
    }

    discover_secure_telemetry(conn_handle);
    return 0;
}

static void exchange_mtu_then_discover(uint16_t conn_handle)
{
    int rc = ble_gattc_exchange_mtu(conn_handle, mtu_exchange_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gattc_exchange_mtu fallo rc=%d; descubriendo sin exchange", rc);
        discover_secure_telemetry(conn_handle);
    }
}

static bool adv_matches_node(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        return false;
    }

    const size_t prefix_len = strlen(BLE_NODE_NAME_PREFIX);
    if (fields.name == NULL || fields.name_len < prefix_len) {
        return false;
    }

    return memcmp(fields.name, BLE_NODE_NAME_PREFIX, prefix_len) == 0;
}

static void connect_to_node(const struct ble_gap_disc_desc *disc)
{
    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc_cancel aviso rc=%d", rc);
    }

    s_current_rssi = disc->rssi;
    s_connecting = true;
    ESP_LOGI(TAG, "Nodo estrella encontrado RSSI=%d; conectando para pairing/bonding/cifrado", (int)s_current_rssi);
    rc = ble_gap_connect(s_own_addr_type,
                         &disc->addr,
                         3000,
                         NULL,
                         ble_gap_event_cb,
                         NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect fallo rc=%d", rc);
        s_connecting = false;
        ble_scanner_start();
    }
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (!s_connecting && s_conn_handle == BLE_HS_CONN_HANDLE_NONE && adv_matches_node(&event->disc)) {
            connect_to_node(&event->disc);
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Conexion BLE establecida. conn_handle=%u", s_conn_handle);
            ESP_LOGI(TAG, "Iniciando seguridad BLE nativa: pairing + bonding + cifrado");
            int rc = ble_gap_security_initiate(s_conn_handle);
            if (rc != 0) {
                ESP_LOGE(TAG, "ble_gap_security_initiate fallo rc=%d", rc);
                ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
        } else {
            ESP_LOGW(TAG, "Conexion fallida status=%d; vuelve el escaneo", event->connect.status);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ble_scanner_start();
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "Enlace BLE cifrado. Pairing/bonding completado o bond reutilizado.");
            exchange_mtu_then_discover(event->enc_change.conn_handle);
        } else {
            ESP_LOGE(TAG, "Fallo cifrado BLE status=%d", event->enc_change.status);
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE desconectado reason=%d; reiniciando escaneo", event->disconnect.reason);
        s_connecting = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_scanner_start();
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        ESP_LOGW(TAG, "Repeat pairing detectado; se permite actualizar bonding");
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (!s_connecting) {
            ESP_LOGI(TAG, "Escaneo BLE completado; reiniciando");
            ble_scanner_start();
        }
        return 0;

    default:
        return 0;
    }
}

static void ble_scanner_start(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE || s_connecting) {
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
        ESP_LOGE(TAG, "ble_gap_disc fallo: %d", rc);
    } else {
        ESP_LOGI(TAG, "Escaneo BLE estrella iniciado. prefix=%s interval=0x%04X window=0x%04X",
                 BLE_NODE_NAME_PREFIX, BLE_SCAN_INTERVAL, BLE_SCAN_WINDOW);
    }
}

static void ble_app_on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE sincronizado. Gateway central listo para topologia estrella BLE segura");
    ble_scanner_start();
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_scanner_init(void)
{
    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init fallo: %d", rc);
        return ESP_FAIL;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    /*
     * Seguridad nativa BLE:
     * LE Secure Connections + bonding + cifrado del enlace.
     * NoInputNoOutput implica Just Works sin MITM, adecuado para estos nodos sin UI.
     */
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    nimble_port_freertos_init(nimble_host_task);
    return ESP_OK;
}
