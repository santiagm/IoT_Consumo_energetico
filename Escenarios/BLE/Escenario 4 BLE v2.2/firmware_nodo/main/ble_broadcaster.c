#include "ble_broadcaster.h"
#include "config.h"
#include "telemetry_packet.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
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

static const char *TAG = "BLE_SEC_GATT";

#define BLE_NODE_DEVICE_NAME        "ESC4_BLE_NODE"
#define BLE_EVT_SYNCED              BIT0
#define BLE_EVT_CONNECTED           BIT1
#define BLE_EVT_ENCRYPTED           BIT2
#define BLE_EVT_PAYLOAD_READ        BIT3

/*
 * UUIDs privados del escenario 4 BLE seguro.
 * Servicio:        9f2a0001-6f91-4f4e-86a1-39f9eaf41c01
 * Caracteristica:  9f2a0002-6f91-4f4e-86a1-39f9eaf41c01
 */
static const ble_uuid128_t s_telemetry_svc_uuid =
    BLE_UUID128_INIT(0x01, 0x1c, 0xf4, 0xea, 0xf9, 0x39, 0xa1, 0x86,
                     0x4e, 0x4f, 0x91, 0x6f, 0x01, 0x00, 0x2a, 0x9f);
static const ble_uuid128_t s_telemetry_chr_uuid =
    BLE_UUID128_INIT(0x01, 0x1c, 0xf4, 0xea, 0xf9, 0x39, 0xa1, 0x86,
                     0x4e, 0x4f, 0x91, 0x6f, 0x02, 0x00, 0x2a, 0x9f);

static EventGroupHandle_t s_ble_events;
static uint8_t s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

static uint8_t s_gatt_payload[4 + sizeof(sensor_packet_t) + AES_GCM_TAG_LEN];
static size_t s_gatt_payload_len;

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg);

static int telemetry_access_cb(uint16_t conn_handle,
                               uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt,
                               void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (s_gatt_payload_len != sizeof(s_gatt_payload)) {
        ESP_LOGE(TAG, "Payload GATT con tamano invalido: %u", (unsigned)s_gatt_payload_len);
        return BLE_ATT_ERR_UNLIKELY;
    }

    int rc = os_mbuf_append(ctxt->om, s_gatt_payload, s_gatt_payload_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "No se pudo copiar payload a GATT rc=%d", rc);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    ESP_LOGI(TAG, "Payload cifrado entregado por GATT seguro. conn=%u bytes=%u",
             conn_handle, (unsigned)s_gatt_payload_len);
    xEventGroupSetBits(s_ble_events, BLE_EVT_PAYLOAD_READ);
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_telemetry_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_telemetry_chr_uuid.u,
                .access_cb = telemetry_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            },
            { 0 }
        },
    },
    { 0 }
};

static void start_advertising(void)
{
    int rc;

    /*
     * IMPORTANTE:
     * Este advertising NO transporta telemetria.
     * Solo es el anuncio minimo conectable para que el Gateway descubra al nodo
     * e inicie la conexion BLE segura. El payload real viaja por GATT cifrado.
     *
     * Se usa RAW advertising para evitar BLE_HS_EINVAL / rc=8 con
     * ble_gap_adv_set_fields() cuando Extended Advertising queda habilitado
     * en sdkconfig o cuando NimBLE rechaza los campos generados.
     */
    /*
     * Nombre incluido directamente en el advertising primario.
     * Esto evita depender del scan response y acelera que el gateway reconozca el nodo.
     */
    const uint8_t adv_data[] = {
        0x02, 0x01, (BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP),
        0x0E, 0x09,
        'E', 'S', 'C', '4', '_', 'B', 'L', 'E', '_', 'N', 'O', 'D', 'E',
    };

    rc = ble_gap_adv_set_data(adv_data, sizeof(adv_data));
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_data RAW fallo rc=%d", rc);
        return;
    }

    const size_t name_len = strlen(BLE_NODE_DEVICE_NAME);
    if ((name_len + 2U) > 31U) {
        ESP_LOGE(TAG, "Nombre BLE demasiado largo para scan response: %u", (unsigned)name_len);
        return;
    }

    uint8_t scan_rsp[31];
    memset(scan_rsp, 0, sizeof(scan_rsp));
    scan_rsp[0] = (uint8_t)(name_len + 1U);
    scan_rsp[1] = 0x09; /* Complete Local Name */
    memcpy(&scan_rsp[2], BLE_NODE_DEVICE_NAME, name_len);

    rc = ble_gap_adv_rsp_set_data(scan_rsp, (int)(name_len + 2U));
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_data RAW fallo rc=%d", rc);
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

    rc = ble_gap_adv_start(s_own_addr_type,
                           NULL,
                           BLE_HS_FOREVER,
                           &adv_params,
                           ble_gap_event_cb,
                           NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start fallo rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Advertising minimo conectable iniciado; telemetria via GATT seguro");
    }
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Gateway conectado. conn_handle=%u", s_conn_handle);
            xEventGroupSetBits(s_ble_events, BLE_EVT_CONNECTED);
        } else {
            ESP_LOGW(TAG, "Conexion fallida status=%d; reiniciando advertising", event->connect.status);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE desconectado. reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "Enlace BLE cifrado activo. Pairing/bonding completado si era requerido.");
            xEventGroupSetBits(s_ble_events, BLE_EVT_ENCRYPTED);
        } else {
            ESP_LOGE(TAG, "Fallo al cifrar enlace BLE status=%d", event->enc_change.status);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        ESP_LOGW(TAG, "Repeat pairing detectado; se permite actualizar bonding");
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertising completado; reiniciando mientras el nodo siga despierto");
        start_advertising();
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

    ESP_LOGI(TAG, "NimBLE sincronizado. Seguridad nativa: pairing + bonding + cifrado + LE Secure Connections");
    xEventGroupSetBits(s_ble_events, BLE_EVT_SYNCED);
}

static void ble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "BLE host task iniciada");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_broadcaster_init(void)
{
    if (s_ble_events == NULL) {
        s_ble_events = xEventGroupCreate();
        if (s_ble_events == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xEventGroupClearBits(s_ble_events,
                         BLE_EVT_SYNCED | BLE_EVT_CONNECTED | BLE_EVT_ENCRYPTED | BLE_EVT_PAYLOAD_READ);

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init fallo: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(BLE_NODE_DEVICE_NAME);

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg fallo rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs fallo rc=%d", rc);
        return ESP_FAIL;
    }

    /*
     * Seguridad nativa BLE:
     * - Pairing y bonding habilitados.
     * - Cifrado de enlace requerido por la caracteristica GATT READ_ENC.
     * - LE Secure Connections habilitado. Con NoInputNoOutput se usa Just Works,
     *   por lo que no hay MITM; es el maximo practico sin teclado/display.
     */
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    nimble_port_freertos_init(ble_host_task);

    EventBits_t bits = xEventGroupWaitBits(s_ble_events,
                                           BLE_EVT_SYNCED,
                                           pdFALSE,
                                           pdTRUE,
                                           pdMS_TO_TICKS(1000));
    if ((bits & BLE_EVT_SYNCED) == 0) {
        ESP_LOGE(TAG, "Timeout esperando sincronizacion NimBLE");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t tx_ret = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P6);
    if (tx_ret != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo configurar BLE TX DEFAULT: %s", esp_err_to_name(tx_ret));
    }

    tx_ret = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P6);
    if (tx_ret != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo configurar BLE TX ADV: %s", esp_err_to_name(tx_ret));
    }

    ESP_LOGW(TAG, "CONFIG REAL NODO: STREAM_SAMPLE_COUNT=%d BLE_SEND_TIMEOUT_MS=%d TX_PWR=P6",
             STREAM_SAMPLE_COUNT, BLE_SEND_TIMEOUT_MS);

    return ESP_OK;
}

esp_err_t ble_broadcaster_send_packet(uint16_t batch_id,
                                      const uint8_t *encrypted_data,
                                      size_t encrypted_len,
                                      const uint8_t *auth_tag)
{
    if (encrypted_data == NULL || auth_tag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (encrypted_len != sizeof(sensor_packet_t)) {
        ESP_LOGE(TAG, "Tamano ciphertext invalido: %u esperado=%u",
                 (unsigned)encrypted_len, (unsigned)sizeof(sensor_packet_t));
        return ESP_ERR_INVALID_SIZE;
    }

    size_t idx = 0;
    s_gatt_payload[idx++] = (uint8_t)(BLE_COMPANY_ID & 0xFF);
    s_gatt_payload[idx++] = (uint8_t)((BLE_COMPANY_ID >> 8) & 0xFF);
    s_gatt_payload[idx++] = (uint8_t)(batch_id & 0xFF);
    s_gatt_payload[idx++] = (uint8_t)((batch_id >> 8) & 0xFF);
    memcpy(&s_gatt_payload[idx], encrypted_data, encrypted_len);
    idx += encrypted_len;
    memcpy(&s_gatt_payload[idx], auth_tag, AES_GCM_TAG_LEN);
    idx += AES_GCM_TAG_LEN;
    s_gatt_payload_len = idx;

    xEventGroupClearBits(s_ble_events, BLE_EVT_CONNECTED | BLE_EVT_ENCRYPTED | BLE_EVT_PAYLOAD_READ);

    ESP_LOGI(TAG, "Payload AES-GCM preparado. batch_id=%u bytes=%u", batch_id, (unsigned)s_gatt_payload_len);
    ESP_LOGW(TAG, "CONFIG REAL BLE_SEND_TIMEOUT_MS=%d ms", BLE_SEND_TIMEOUT_MS);
    start_advertising();

    EventBits_t bits = xEventGroupWaitBits(s_ble_events,
                                           BLE_EVT_PAYLOAD_READ,
                                           pdFALSE,
                                           pdTRUE,
                                           pdMS_TO_TICKS(BLE_SEND_TIMEOUT_MS));

    if ((bits & BLE_EVT_PAYLOAD_READ) == 0) {
        ESP_LOGW(TAG, "Timeout esperando lectura GATT segura del Gateway");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Envio BLE seguro completado. batch_id=%u", batch_id);

    /*
     * El callback de lectura GATT ya copio el payload al buffer ATT y marco
     * BLE_EVT_PAYLOAD_READ. Sin esta espera, app_main() llama enseguida a
     * ble_broadcaster_stop(), termina la conexion y el gateway puede recibir
     * status=7 en la lectura porque el enlace se corta antes de procesar la
     * respuesta completa.
     */
    ESP_LOGW(TAG, "Esperando %d ms antes de cerrar conexion BLE/GATT",
             BLE_POST_READ_GRACE_MS);
    vTaskDelay(pdMS_TO_TICKS(BLE_POST_READ_GRACE_MS));

    return ESP_OK;
}

void ble_broadcaster_stop(void)
{
    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_adv_stop aviso rc=%d", rc);
    }

    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_gap_terminate aviso rc=%d", rc);
        }
    }

    /* Deep sleep apaga radio/controlador; no se fuerza nimble_port_deinit(). */
}
