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
#include "ezbee/zdo.h"
#include "ezbee/zha.h"
#include "ezbee/platform/radio.h"
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
    char src_mac[18];
    uint16_t last_batch;
    bool used;
} node_duplicate_state_t;





static node_duplicate_state_t s_node_states[AUTHORIZED_NODE_COUNT];

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

static const char *safe_node_id(const sensor_packet_t *pkt)
{
    if (pkt == NULL || pkt->node_id[0] == '\0') {
        return "NX";
    }

    return pkt->node_id;
}

static bool is_duplicate_packet(const char *src_mac, uint16_t batch_id)
{
    if (src_mac == NULL || src_mac[0] == '\0') {
        return false;
    }

    for (uint8_t i = 0; i < AUTHORIZED_NODE_COUNT; i++) {
        if (s_node_states[i].used && strncmp(s_node_states[i].src_mac, src_mac, sizeof(s_node_states[i].src_mac)) == 0) {
            bool duplicate = (s_node_states[i].last_batch == batch_id);
            s_node_states[i].last_batch = batch_id;
            return duplicate;
        }
    }

    for (uint8_t i = 0; i < AUTHORIZED_NODE_COUNT; i++) {
        if (!s_node_states[i].used) {
            snprintf(s_node_states[i].src_mac, sizeof(s_node_states[i].src_mac), "%s", src_mac);
            s_node_states[i].last_batch = batch_id;
            s_node_states[i].used = true;
            return false;
        }
    }

    /* Si llegan más nodos de los autorizados, no se marca duplicado por lote global. */
    return false;
}

static esp_err_t sensor_packet_to_json_uart(const sensor_packet_t *pkt,
                                            bool duplicate,
                                            int8_t rssi,
                                            int lqi,
                                            char *json,
                                            size_t json_len)
{
    if (pkt == NULL || json == NULL || json_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t ts_gateway_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    const uint32_t node_tx_timestamp_ms = pkt->start_timestamp_ms;
    const uint32_t latency_ms = (ts_gateway_ms >= node_tx_timestamp_ms)
                                ? (ts_gateway_ms - node_tx_timestamp_ms)
                                : 0;

    const float synthetic_ax = accel_raw_to_g(pkt->muestras[0].ax);
    const float synthetic_ay = accel_raw_to_g(pkt->muestras[0].ay);
    const float synthetic_az = accel_raw_to_g(pkt->muestras[0].az);
    const float synthetic_gx = gyro_raw_to_dps(pkt->muestras[0].gx);
    const float synthetic_gy = gyro_raw_to_dps(pkt->muestras[0].gy);
    const float synthetic_gz = gyro_raw_to_dps(pkt->muestras[0].gz);

    int n = snprintf(json,
                     json_len,
                     "{\"node_id\":\"%s\",\"src_mac\":\"%s\",\"packet_id\":%u,\"batch_id\":%u,"
                     "\"ts_gateway_ms\":%" PRIu32 ",\"node_tx_timestamp_ms\":%" PRIu32 ",\"latency_ms\":%" PRIu32 ","
                     "\"rssi\":%d,\"lqi\":%d,\"duplicate\":%s,\"decrypt_ok\":true,"
                     "\"values\":{\"synthetic_ax\":%.5f,\"synthetic_ay\":%.5f,\"synthetic_az\":%.5f,"
                     "\"synthetic_gx\":%.5f,\"synthetic_gy\":%.5f,\"synthetic_gz\":%.5f}}\n",
                     safe_node_id(pkt),
                     pkt->src_mac,
                     (unsigned)pkt->batch_id,
                     (unsigned)pkt->batch_id,
                     ts_gateway_ms,
                     node_tx_timestamp_ms,
                     latency_ms,
                     rssi,
                     lqi,
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

static void handle_payload(const uint8_t *payload, uint16_t len, int8_t rssi, int lqi)
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

    if (pkt.src_mac[0] == '\0') {
        s_metrics.inv++;
        ESP_LOGW(TAG, "sensor_packet_t invalido: src_mac vacio");
        print_metrics();
        return;
    }

    bool duplicate = is_duplicate_packet(pkt.src_mac, pkt.batch_id);
    if (duplicate) {
        s_metrics.dup++;
    }

    s_metrics.rx++;

    ESP_LOGI(TAG,
             "sensor_packet_t aceptado node_id=%s src_mac=%s timestamp=%" PRIu32 " batch=%u duplicate=%s",
             safe_node_id(&pkt),
             pkt.src_mac,
             pkt.start_timestamp_ms,
             (unsigned)pkt.batch_id,
             duplicate ? "true" : "false");

    char json[UART_BRIDGE_MAX_JSON_LEN];

    err = sensor_packet_to_json_uart(&pkt, duplicate, rssi, lqi, json, sizeof(json));

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
                 "JSON enviado a Gateway MQTT por UART batch=%u",
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
     * El LQI real usable lo buscamos en la tabla de vecinos NWK.
     * Para eso necesitamos que el paquete venga con dirección corta.
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
             "No se encontró LQI en tabla de vecinos para src_short=0x%04hx",
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
    * 1) header_rssi: si el callback ZCL trae RSSI real.
    * 2) neighbor_rssi: RSSI de la tabla de vecinos, coherente con lqi_nwk.
    * 3) radio_rssi: solo respaldo, porque puede no corresponder exactamente
    *    al paquete actual.
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

typedef struct {
    const char *node_id;
    uint8_t ieee[8];
    const char *install_code_hex;
} authorized_node_config_t;

static esp_err_t configure_install_code_after_zigbee_init(void)
{
    authorized_node_config_t authorized_nodes[AUTHORIZED_NODE_COUNT] = {
        {AUTHORIZED_NODE1_ID, AUTHORIZED_NODE1_IEEE_ADDR, AUTHORIZED_NODE1_INSTALL_CODE_HEX},
        {AUTHORIZED_NODE2_ID, AUTHORIZED_NODE2_IEEE_ADDR, AUTHORIZED_NODE2_INSTALL_CODE_HEX},
        {AUTHORIZED_NODE3_ID, AUTHORIZED_NODE3_IEEE_ADDR, AUTHORIZED_NODE3_INSTALL_CODE_HEX},
    };

    ESP_LOGI(TAG, "Configurando seguridad Install Code en Trust Center para %u nodos",
             (unsigned)AUTHORIZED_NODE_COUNT);

    ESP_RETURN_ON_ERROR(install_code_trust_center_require(true),
                        TAG,
                        "No se pudo habilitar Install Code requerido");

    for (uint8_t i = 0; i < AUTHORIZED_NODE_COUNT; i++) {
        ESP_LOGI(TAG,
                 "Registrando Install Code %s ieee={0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X}",
                 authorized_nodes[i].node_id,
                 authorized_nodes[i].ieee[0], authorized_nodes[i].ieee[1],
                 authorized_nodes[i].ieee[2], authorized_nodes[i].ieee[3],
                 authorized_nodes[i].ieee[4], authorized_nodes[i].ieee[5],
                 authorized_nodes[i].ieee[6], authorized_nodes[i].ieee[7]);

        ESP_RETURN_ON_ERROR(install_code_trust_center_add_128(
                                authorized_nodes[i].ieee,
                                authorized_nodes[i].install_code_hex),
                            TAG,
                            "No se pudo registrar Install Code de un nodo estrella");
    }

    ESP_LOGI(TAG, "Install Code requerido habilitado para topologia estrella");

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