#include "zigbee_receiver.h"
#include "config.h"
#include "telemetry_packet.h"
#include "uart_bridge_sender.h"
#include "install_code_compat.h"
#include "app_crypto.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_zigbee.h"

#include "ezbee/af.h"
#include "ezbee/app_signals.h"
#include "ezbee/bdb.h"
#include "ezbee/nwk.h"
#include "ezbee/platform/radio.h"
#include "ezbee/zdo.h"
#include "ezbee/zha.h"

#include "ezbee/zcl.h"
#include "ezbee/zcl/zcl_common.h"
#include "ezbee/zcl/zcl_core.h"
#include "ezbee/zcl/zcl_desc.h"
#include "ezbee/zcl/zcl_type.h"

#include "ezbee/zcl/cluster/basic.h"
#include "ezbee/zcl/cluster/basic_desc.h"
#include "ezbee/zcl/cluster/identify.h"
#include "ezbee/zcl/cluster/identify_desc.h"
#include "ezbee/zcl/cluster/custom.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>

static const char *TAG = "ZB_GATEWAY";

static zigbee_gateway_metrics_t s_metrics;

typedef struct {
    char node_id[4];
    uint32_t last_packet_id;
    uint32_t last_batch_id;
    bool valid;
} node_rx_state_t;

static node_rx_state_t s_node_states[] = {
    {.node_id = "N1", .valid = false},
    {.node_id = "N2", .valid = false},
    {.node_id = "N3", .valid = false},
};


/*
 * Compatibilidad de nombres entre versiones del config.h.
 */
#ifndef DEVICE_LOGICAL_NAME
#define DEVICE_LOGICAL_NAME              "tesis-zigbee-gateway"
#endif

#ifndef ZIGBEE_GATEWAY_ENDPOINT
#ifdef ZIGBEE_ENDPOINT_GATEWAY
#define ZIGBEE_GATEWAY_ENDPOINT          ZIGBEE_ENDPOINT_GATEWAY
#else
#define ZIGBEE_GATEWAY_ENDPOINT          1
#endif
#endif

#ifndef ZIGBEE_SENSOR_ENDPOINT
#ifdef ZIGBEE_ENDPOINT_NODE
#define ZIGBEE_SENSOR_ENDPOINT           ZIGBEE_ENDPOINT_NODE
#else
#define ZIGBEE_SENSOR_ENDPOINT           10
#endif
#endif

#ifndef ZIGBEE_TELEMETRY_CMD_ID
#ifdef ZIGBEE_CUSTOM_COMMAND_ID
#define ZIGBEE_TELEMETRY_CMD_ID          ZIGBEE_CUSTOM_COMMAND_ID
#else
#define ZIGBEE_TELEMETRY_CMD_ID          0x01
#endif
#endif

#ifndef ZIGBEE_PRIMARY_CHANNEL_MASK
#define ZIGBEE_PRIMARY_CHANNEL_MASK      (1UL << ZIGBEE_CHANNEL)
#endif

#ifndef ZIGBEE_MANUFACTURER_CODE
#define ZIGBEE_MANUFACTURER_CODE         0x1234
#endif

#ifndef UART_BRIDGE_MAX_JSON_LEN
#define UART_BRIDGE_MAX_JSON_LEN         1024
#endif

/*
 * Atributo dummy obligatorio para que el custom cluster tenga descriptor.
 * El payload real llega por comando custom.
 */
#define ZIGBEE_CUSTOM_ATTR_ID            0x0000
#define ZIGBEE_CUSTOM_DEVICE_ID          0xFF02
#define ZIGBEE_RX_RSSI_UNAVAILABLE       (-1)
#define ZIGBEE_RX_LQI_UNAVAILABLE        (-1)

const zigbee_gateway_metrics_t *zigbee_receiver_metrics(void)
{
    return &s_metrics;
}

static void print_metrics(void)
{
    ESP_LOGI(TAG,
             "Metricas RX=%lu UART=%lu DUP=%lu INV=%lu DEC_ERR=%lu UART_ERR=%lu",
             (unsigned long)s_metrics.rx,
             (unsigned long)s_metrics.uart,
             (unsigned long)s_metrics.dup,
             (unsigned long)s_metrics.inv,
             (unsigned long)s_metrics.dec_err,
             (unsigned long)s_metrics.uart_err);
}

static float accel_raw_to_g(int16_t raw)
{
    return raw / 16384.0f;
}

static float gyro_raw_to_dps(int16_t raw)
{
    return raw / 131.0f;
}

static bool node_state_mark_and_check_duplicate(const sensor_packet_t *pkt)
{
    if (pkt == NULL || pkt->node_id[0] == '\0') {
        return false;
    }

    for (size_t i = 0; i < (sizeof(s_node_states) / sizeof(s_node_states[0])); i++) {
        if (strncmp(s_node_states[i].node_id, pkt->node_id, sizeof(pkt->node_id)) == 0) {
            bool duplicate = s_node_states[i].valid &&
                             s_node_states[i].last_packet_id == pkt->packet_id &&
                             s_node_states[i].last_batch_id == pkt->batch_id;

            s_node_states[i].last_packet_id = pkt->packet_id;
            s_node_states[i].last_batch_id = pkt->batch_id;
            s_node_states[i].valid = true;

            return duplicate;
        }
    }

    return false;
}

static esp_err_t sensor_packet_to_json_uart(const sensor_packet_t *pkt,
                                            bool duplicate,
                                            int8_t rx_rssi,
                                            int rx_lqi,
                                            char *json,
                                            size_t json_len)
{
    if (pkt == NULL || json == NULL || json_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char node_id[4] = {0};
    memcpy(node_id, pkt->node_id, sizeof(pkt->node_id));
    node_id[sizeof(node_id) - 1] = '\0';

    int64_t ts_gateway_ms64 = esp_timer_get_time() / 1000LL;
    uint32_t ts_gateway_ms = (uint32_t)ts_gateway_ms64;
    uint32_t latency_ms = 0;

    if (ts_gateway_ms >= pkt->start_timestamp_ms) {
        latency_ms = ts_gateway_ms - pkt->start_timestamp_ms;
    }

    const synthetic_sample_t *sample = &pkt->muestras[0];

    float synthetic_ax = accel_raw_to_g(sample->ax);
    float synthetic_ay = accel_raw_to_g(sample->ay);
    float synthetic_az = accel_raw_to_g(sample->az);
    float synthetic_gx = gyro_raw_to_dps(sample->gx);
    float synthetic_gy = gyro_raw_to_dps(sample->gy);
    float synthetic_gz = gyro_raw_to_dps(sample->gz);

    int n = snprintf(json,
                     json_len,
                     "{\"node_id\":\"%s\","
                     "\"src_mac\":\"%s\","
                     "\"packet_id\":%" PRIu32 ","
                     "\"batch_id\":%u,"
                     "\"ts_gateway_ms\":%" PRIu32 ","
                     "\"node_tx_timestamp_ms\":%" PRIu32 ","
                     "\"latency_ms\":%" PRIu32 ","
                     "\"rssi\":%d,"
                     "\"lqi\":%d,"
                     "\"duplicate\":%s,"
                     "\"decrypt_ok\":true,"
                     "\"values\":{"
                     "\"synthetic_ax\":%.5f,"
                     "\"synthetic_ay\":%.5f,"
                     "\"synthetic_az\":%.5f,"
                     "\"synthetic_gx\":%.5f,"
                     "\"synthetic_gy\":%.5f,"
                     "\"synthetic_gz\":%.5f"
                     "}}\n",
                     node_id,
                     pkt->src_mac,
                     pkt->packet_id,
                     (unsigned)pkt->batch_id,
                     ts_gateway_ms,
                     pkt->start_timestamp_ms,
                     latency_ms,
                     (int)rx_rssi,
                     rx_lqi,
                     duplicate ? "true" : "false",
                     (double)synthetic_ax,
                     (double)synthetic_ay,
                     (double)synthetic_az,
                     (double)synthetic_gx,
                     (double)synthetic_gy,
                     (double)synthetic_gz);

    if (n < 0 || (size_t)n >= json_len) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void handle_payload(const uint8_t *payload, uint16_t len, int8_t rx_rssi, int rx_lqi)
{
    if (payload == NULL || len != sizeof(secure_zigbee_packet_t)) {
        s_metrics.dec_err++;
        ESP_LOGW(TAG,
                 "DEC_ERR/len segura invalida len=%u esperado=%u",
                 (unsigned)len,
                 (unsigned)sizeof(secure_zigbee_packet_t));
        print_metrics();
        return;
    }

    secure_zigbee_packet_t secure_pkt;
    sensor_packet_t pkt;

    memcpy(&secure_pkt, payload, sizeof(secure_pkt));

    if (secure_pkt.magic != SECURE_ZB_MAGIC ||
        secure_pkt.version != SECURE_ZB_VERSION ||
        secure_pkt.encrypted_len != SENSOR_PACKET_SIZE) {
        s_metrics.inv++;
        ESP_LOGW(TAG,
                 "Paquete seguro invalido magic=0x%08" PRIx32 " version=%u encrypted_len=%u",
                 secure_pkt.magic,
                 secure_pkt.version,
                 (unsigned)secure_pkt.encrypted_len);
        print_metrics();
        return;
    }

    ESP_LOGI(TAG,
             "Paquete seguro recibido batch=%u encrypted_len=%u iv=%02X%02X%02X%02X...",
             (unsigned)secure_pkt.batch_id,
             (unsigned)secure_pkt.encrypted_len,
             secure_pkt.iv[0], secure_pkt.iv[1], secure_pkt.iv[2], secure_pkt.iv[3]);

    esp_err_t err = app_crypto_decrypt_sensor_packet(&secure_pkt, &pkt);

    if (err != ESP_OK) {
        s_metrics.dec_err++;
        ESP_LOGW(TAG,
                 "AES-GCM_ERR paquete descartado batch=%u err=%s",
                 (unsigned)secure_pkt.batch_id,
                 esp_err_to_name(err));
        print_metrics();
        return;
    }

    pkt.node_id[sizeof(pkt.node_id) - 1] = '\0';

    if (pkt.src_mac[0] == '\0' || pkt.node_id[0] == '\0') {
        s_metrics.inv++;
        ESP_LOGW(TAG, "sensor_packet_t invalido: src_mac o node_id vacio");
        print_metrics();
        return;
    }

    bool duplicate = node_state_mark_and_check_duplicate(&pkt);

    if (duplicate) {
        s_metrics.dup++;
    }

    s_metrics.rx++;

    ESP_LOGI(TAG,
             "sensor_packet_t aceptado node_id=%s src_mac=%s packet_id=%" PRIu32 " timestamp=%" PRIu32 " batch=%u duplicate=%s",
             pkt.node_id,
             pkt.src_mac,
             pkt.packet_id,
             pkt.start_timestamp_ms,
             (unsigned)pkt.batch_id,
             duplicate ? "true" : "false");

    char json[UART_BRIDGE_MAX_JSON_LEN];

    err = sensor_packet_to_json_uart(&pkt, duplicate, rx_rssi, rx_lqi, json, sizeof(json));

    if (err != ESP_OK) {
        s_metrics.dec_err++;
        ESP_LOGW(TAG, "No se pudo generar JSON: %s", esp_err_to_name(err));
        print_metrics();
        return;
    }

    ESP_LOGI(TAG, "JSON generado: %s", json);

    err = uart_bridge_send_json(json);

    if (err == ESP_OK) {
        s_metrics.uart++;
        ESP_LOGI(TAG,
                 "JSON enviado a Gateway MQTT por UART node_id=%s packet_id=%" PRIu32 " batch=%u",
                 pkt.node_id,
                 pkt.packet_id,
                 (unsigned)pkt.batch_id);
    } else {
        s_metrics.uart_err++;
        ESP_LOGE(TAG,
                 "UART_ERR batch=%u err=%s",
                 (unsigned)pkt.batch_id,
                 esp_err_to_name(err));
    }

    print_metrics();
}


static int get_lqi_from_neighbor_table(const ezb_zcl_cmd_hdr_t *header, int8_t *neighbor_rssi)
{
    if (neighbor_rssi != NULL) {
        *neighbor_rssi = 127;
    }

    if (header == NULL) {
        return ZIGBEE_RX_LQI_UNAVAILABLE;
    }

    /*
     * El LQI/RSSI usable se busca en la tabla de vecinos NWK.
     * En malla, esto representa el enlace con el vecino directo del Gateway
     * cuando el short address recibido existe en la tabla de vecinos.
     */
    if (header->src_addr.addr_mode != EZB_ADDR_MODE_SHORT) {
        ESP_LOGW(TAG,
                 "No se puede buscar LQI: src_addr no es SHORT, addr_mode=%u",
                 header->src_addr.addr_mode);
        return ZIGBEE_RX_LQI_UNAVAILABLE;
    }

    ezb_shortaddr_t src_short = header->src_addr.u.short_addr;

    ezb_nwk_info_iterator_t iterator = EZB_NWK_INFO_ITERATOR_INIT;
    ezb_nwk_neighbor_info_t nbr_info = {0};

    while (ezb_nwk_get_next_neighbor(&iterator, &nbr_info) == EZB_ERR_NONE) {
        ESP_LOGI(TAG,
                 "DEBUG_ZB_NEIGHBOR: short=0x%04hx LQI=%u RSSI=%d rel=%u depth=%u age=%u",
                 nbr_info.short_addr,
                 nbr_info.lqi,
                 nbr_info.rssi,
                 nbr_info.relationship,
                 nbr_info.depth,
                 nbr_info.age);

        if (nbr_info.short_addr == src_short) {
            if (neighbor_rssi != NULL) {
                *neighbor_rssi = nbr_info.rssi;
            }

            ESP_LOGI(TAG,
                     "DEBUG_ZB_LQI_MATCH: src_short=0x%04hx LQI=%u RSSI=%d",
                     src_short,
                     nbr_info.lqi,
                     nbr_info.rssi);

            return (int)nbr_info.lqi;
        }
    }

    ESP_LOGW(TAG,
             "No se encontro LQI en tabla de vecinos para src_short=0x%04hx",
             src_short);

    return ZIGBEE_RX_LQI_UNAVAILABLE;
}



static ezb_zcl_status_t custom_cluster_process_cmd_handler(const ezb_zcl_cmd_hdr_t *header,
                                                           const uint8_t *payload,
                                                           uint16_t payload_length)
{
    if (header == NULL || payload == NULL || payload_length == 0) {
        s_metrics.inv++;
        ESP_LOGW(TAG, "Comando custom invalido: header/payload vacio");
        print_metrics();
        return EZB_ZCL_STATUS_INVALID_VALUE;
    }

    const int8_t header_rssi = header->rssi;
    const int8_t radio_rssi = ezb_plat_radio_get_rssi();

    int8_t neighbor_rssi = 127;
    int rx_lqi = get_lqi_from_neighbor_table(header, &neighbor_rssi);

    int8_t rx_rssi = ZIGBEE_RX_RSSI_UNAVAILABLE;

    /*
    * Prioridad correcta:
    * 1) header_rssi, si viene válido.
    * 2) neighbor_rssi, coherente con lqi_nwk.
    * 3) radio_rssi, solo como respaldo.
    */
    if (header_rssi != ZIGBEE_RX_RSSI_UNAVAILABLE && header_rssi != 127) {
        rx_rssi = header_rssi;
    } else if (neighbor_rssi != 127) {
        rx_rssi = neighbor_rssi;
    } else if (radio_rssi != 127) {
        rx_rssi = radio_rssi;
    }

    ESP_LOGI(TAG,
            "Custom cmd recibido cluster=0x%04x cmd=0x%02x src_ep=%u dst_ep=%u len=%u header_rssi=%d radio_rssi=%d neighbor_rssi=%d rssi_usado=%d lqi_nwk=%d",
            header->cluster_id,
            header->cmd_id,
            header->src_ep,
            header->dst_ep,
            payload_length,
            (int)header_rssi,
            (int)radio_rssi,
            (int)neighbor_rssi,
            (int)rx_rssi,
            rx_lqi);

    if (header->cluster_id != ZIGBEE_CUSTOM_CLUSTER_ID ||
        header->cmd_id != ZIGBEE_TELEMETRY_CMD_ID) {
        s_metrics.inv++;
        ESP_LOGW(TAG,
                 "Custom cmd no soportado cluster=0x%04x cmd=0x%02x",
                 header->cluster_id,
                 header->cmd_id);
        print_metrics();
        return EZB_ZCL_STATUS_UNSUP_CMD;
    }

    handle_payload(payload, payload_length, rx_rssi, rx_lqi);

    return EZB_ZCL_STATUS_SUCCESS;
}

static void custom_server_cluster_init(uint8_t ep_id)
{
    ESP_LOGI(TAG,
             "Inicializando custom cluster server endpoint=%u cluster=0x%04x",
             ep_id,
             ZIGBEE_CUSTOM_CLUSTER_ID);

    ezb_zcl_custom_cluster_handlers_t custom_handlers = {
        .cluster_id     = ZIGBEE_CUSTOM_CLUSTER_ID,
        .cluster_role   = EZB_ZCL_CLUSTER_SERVER,
        .process_cmd_cb = custom_cluster_process_cmd_handler,
        .check_value_cb = NULL,
        .write_attr_cb  = NULL,
        .cmd_disc_cb    = NULL,
    };

    ezb_zcl_custom_cluster_handlers_register(&custom_handlers);
}

static esp_err_t create_custom_server_device(uint8_t ep_id)
{
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();
    ezb_af_ep_desc_t ep_desc = NULL;

    ezb_zcl_cluster_desc_t basic_desc = NULL;
    ezb_zcl_cluster_desc_t identify_desc = NULL;
    ezb_zcl_cluster_desc_t custom_desc = NULL;

    ezb_zcl_basic_cluster_server_config_t basic_cfg = {
        .zcl_version = EZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = EZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
    };

    basic_desc = ezb_zcl_basic_create_cluster_desc(&basic_cfg, EZB_ZCL_CLUSTER_SERVER);
    ESP_RETURN_ON_FALSE(basic_desc != NULL, ESP_FAIL, TAG, "No se pudo crear Basic cluster");

    uint8_t manufacturer[] = "\x09" "ESPRESSIF";
    uint8_t model[] = "\x14" DEVICE_LOGICAL_NAME;

    ESP_ERROR_CHECK(ezb_zcl_basic_cluster_desc_add_attr(
        basic_desc,
        EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
        manufacturer));

    ESP_ERROR_CHECK(ezb_zcl_basic_cluster_desc_add_attr(
        basic_desc,
        EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
        model));

    ezb_zcl_identify_cluster_server_config_t identify_cfg = {
        .identify_time = EZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };

    identify_desc = ezb_zcl_identify_create_cluster_desc(&identify_cfg, EZB_ZCL_CLUSTER_SERVER);
    ESP_RETURN_ON_FALSE(identify_desc != NULL, ESP_FAIL, TAG, "No se pudo crear Identify cluster");

    ezb_zcl_custom_cluster_config_t custom_cfg = {
        .cluster_id = ZIGBEE_CUSTOM_CLUSTER_ID,
        .init_func = custom_server_cluster_init,
        .deinit_func = NULL,
    };

    custom_desc = ezb_zcl_custom_create_cluster_desc(&custom_cfg, EZB_ZCL_CLUSTER_SERVER);
    ESP_RETURN_ON_FALSE(custom_desc != NULL, ESP_FAIL, TAG, "No se pudo crear Custom cluster");

    static uint8_t custom_name[] = "\x0d" "tesis-zigbee";

    ESP_ERROR_CHECK(ezb_zcl_custom_cluster_desc_add_attr(
        custom_desc,
        ZIGBEE_CUSTOM_ATTR_ID,
        EZB_ZCL_ATTR_TYPE_STRING,
        EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING,
        custom_name));

    ezb_af_ep_config_t ep_config = {
        .ep_id = ep_id,
        .app_profile_id = EZB_AF_HA_PROFILE_ID,
        .app_device_id = ZIGBEE_CUSTOM_DEVICE_ID,
        .app_device_version = 0,
    };

    ep_desc = ezb_af_create_endpoint_desc(&ep_config);
    ESP_RETURN_ON_FALSE(ep_desc != NULL, ESP_FAIL, TAG, "No se pudo crear endpoint desc");

    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, basic_desc));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, identify_desc));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, custom_desc));

    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev_desc));

    ESP_LOGI(TAG,
             "Endpoint Zigbee registrado endpoint=%u custom_cluster=0x%04x",
             ep_id,
             ZIGBEE_CUSTOM_CLUSTER_ID);

    return ESP_OK;
}

static bool app_signal_handler(const ezb_app_signal_t *signal)
{
    ezb_app_signal_type_t sig_type = ezb_app_signal_get_type(signal);
    const void *params = ezb_app_signal_get_params(signal);

    switch (sig_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Iniciando Zigbee Coordinator");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        return true;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
        ESP_LOGI(TAG, "BDB start/reboot signal=0x%04x", sig_type);

        if (ezb_bdb_is_factory_new()) {
            ESP_LOGI(TAG, "Factory new: formando red Zigbee");
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
        } else {
            ESP_LOGI(TAG, "No factory new: abriendo permit join");
            ezb_bdb_open_network(ZIGBEE_PERMIT_JOIN_SECONDS);
            ESP_LOGI(TAG, "Permit join habilitado");
        }
        return true;

    case EZB_BDB_SIGNAL_FORMATION:
        ESP_LOGI(TAG,
                 "Red Zigbee creada PAN_ID=0x%04hx channel=%u",
                 ezb_nwk_get_panid(),
                 ezb_nwk_get_current_channel());

        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        return true;

    case EZB_BDB_SIGNAL_STEERING:
        ESP_LOGI(TAG, "Network steering finalizado, abriendo permit join");
        ezb_bdb_open_network(ZIGBEE_PERMIT_JOIN_SECONDS);
        ESP_LOGI(TAG, "Permit join habilitado");
        return true;

    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const ezb_zdo_signal_device_annce_params_t *dev =
            (const ezb_zdo_signal_device_annce_params_t *)params;

        if (dev) {
            ESP_LOGI(TAG,
                     "Nodo autorizado/unido short=0x%04hx",
                     dev->short_addr);
        } else {
            ESP_LOGI(TAG, "Nodo autorizado/unido");
        }
        return true;
    }

    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        const ezb_nwk_signal_permit_join_status_params_t *join =
            (const ezb_nwk_signal_permit_join_status_params_t *)params;

        if (join) {
            ESP_LOGI(TAG,
                     "Network(0x%04hx) permit join=%u",
                     ezb_nwk_get_panid(),
                     join->duration);
        } else {
            ESP_LOGI(TAG, "Permit join status recibido");
        }
        return true;
    }

    default:
        ESP_LOGI(TAG,
                 "Zigbee signal=%s (0x%04x)",
                 ezb_app_signal_to_string(sig_type),
                 sig_type);
        return false;
    }
}

static esp_err_t configure_install_code_after_zigbee_init(void)
{
    static const uint8_t authorized_ieee[3][8] = {
        AUTHORIZED_N1_IEEE_ADDR,
        AUTHORIZED_N2_IEEE_ADDR,
        AUTHORIZED_N3_IEEE_ADDR,
    };

    static const char *authorized_ic[3] = {
        AUTHORIZED_N1_INSTALL_CODE_HEX,
        AUTHORIZED_N2_INSTALL_CODE_HEX,
        AUTHORIZED_N3_INSTALL_CODE_HEX,
    };

    static const char *authorized_node_id[3] = {
        "N1",
        "N2",
        "N3",
    };

    ESP_LOGI(TAG, "Configurando seguridad Install Code en Trust Center para N1/N2/N3");

    ESP_RETURN_ON_ERROR(install_code_trust_center_require(true),
                        TAG,
                        "No se pudo habilitar Install Code requerido");

    for (size_t i = 0; i < 3; i++) {
        ESP_LOGI(TAG,
                 "Registrando Install Code de %s en Trust Center",
                 authorized_node_id[i]);

        ESP_RETURN_ON_ERROR(install_code_trust_center_add_128(
                                authorized_ieee[i],
                                authorized_ic[i]),
                            TAG,
                            "No se pudo registrar Install Code de un nodo");
    }

    ESP_LOGI(TAG, "Install Code requerido habilitado para N1/N2/N3");

    return ESP_OK;
}

static void zigbee_task(void *pv)
{
    ESP_LOGI(TAG, "Iniciando Zigbee Coordinator / Trust Center");

    esp_zigbee_config_t zb_cfg = {
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR,
            .install_code_policy = true,
            .zczr_config = {
                .max_children = 10,
            },
        },
        .platform_config = {
            .storage_partition_name = "zb_storage",
            .radio_config = {
                .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,
            },
        },
    };

    ESP_ERROR_CHECK(esp_zigbee_init(&zb_cfg));

    /*
     * Install Code debe configurarse despues de esp_zigbee_init().
     * Antes de esto puede provocar Guru Meditation / assert.
     */
    esp_err_t ic_err = configure_install_code_after_zigbee_init();

    if (ic_err != ESP_OK) {
        ESP_LOGE(TAG,
                "Fallo configurando Install Code. err=%s. Revisar CRC, IEEE address o formato.",
                esp_err_to_name(ic_err));

        /*
        * No continuamos si el Install Code no se registra.
        * Pero tampoco abortamos con Guru Meditation.
        */
        vTaskDelay(pdMS_TO_TICKS(1000));
        vTaskDelete(NULL);
        return;
    }

    ezb_nwk_set_panid(ZIGBEE_PAN_ID);
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_set_channel_mask(ZIGBEE_PRIMARY_CHANNEL_MASK)));

    ESP_ERROR_CHECK(create_custom_server_device(ZIGBEE_GATEWAY_ENDPOINT));

    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_app_signal_add_handler(app_signal_handler)));

    ESP_LOGI(TAG,
             "Red Zigbee configurada PAN_ID=0x%04x channel=%d",
             ZIGBEE_PAN_ID,
             ZIGBEE_CHANNEL);

    ESP_ERROR_CHECK(esp_zigbee_start(false));

    ESP_ERROR_CHECK(esp_zigbee_launch_mainloop());

    ESP_ERROR_CHECK(esp_zigbee_deinit());

    vTaskDelete(NULL);
}

esp_err_t zigbee_receiver_start(void)
{
    BaseType_t ok = xTaskCreate(zigbee_task,
                                "Zigbee_main",
                                8192,
                                NULL,
                                5,
                                NULL);

    ESP_RETURN_ON_FALSE(ok == pdPASS,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "No se pudo crear tarea Zigbee");

    return ESP_OK;
}
