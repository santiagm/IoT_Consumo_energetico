#include "zigbee_sender.h"
#include "config.h"
#include "telemetry_packet.h"
#include "install_code_compat.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_zigbee.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ezbee/af.h"
#include "ezbee/app_signals.h"
#include "ezbee/bdb.h"
#include "ezbee/nwk.h"
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

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#ifndef DEVICE_LOGICAL_NAME
#define DEVICE_LOGICAL_NAME "tesis-zigbee-nodo"
#endif

#ifndef ZIGBEE_SENSOR_ENDPOINT
#ifdef ZIGBEE_ENDPOINT_NODE
#define ZIGBEE_SENSOR_ENDPOINT ZIGBEE_ENDPOINT_NODE
#else
#define ZIGBEE_SENSOR_ENDPOINT 10
#endif
#endif

#ifndef ZIGBEE_GATEWAY_ENDPOINT
#ifdef ZIGBEE_ENDPOINT_GATEWAY
#define ZIGBEE_GATEWAY_ENDPOINT ZIGBEE_ENDPOINT_GATEWAY
#else
#define ZIGBEE_GATEWAY_ENDPOINT 1
#endif
#endif

#ifndef ZIGBEE_TELEMETRY_CMD_ID
#ifdef ZIGBEE_CUSTOM_COMMAND_ID
#define ZIGBEE_TELEMETRY_CMD_ID ZIGBEE_CUSTOM_COMMAND_ID
#else
#define ZIGBEE_TELEMETRY_CMD_ID 0x01
#endif
#endif

#ifndef ZIGBEE_PRIMARY_CHANNEL_MASK
#define ZIGBEE_PRIMARY_CHANNEL_MASK (1UL << ZIGBEE_CHANNEL)
#endif

#ifndef ZIGBEE_TX_POWER_DBM
#define ZIGBEE_TX_POWER_DBM 0
#endif

static const char *TAG = "ZB_NODE";

static volatile bool s_joined = false;

#define ZIGBEE_CUSTOM_DEVICE_ID        0xFF01
#define ZIGBEE_CUSTOM_ATTR_ID          0x0000
#define ZIGBEE_ED_KEEPALIVE_MS         3000

static void commissioning_start_cb(void *ctx)
{
    uint32_t mode = (uint32_t)(uintptr_t)ctx;

    ezb_err_t ret = ezb_bdb_start_top_level_commissioning((uint8_t)mode);

    if (ret != EZB_ERR_NONE) {
        ESP_LOGW(TAG,
                 "No se pudo iniciar commissioning mode=0x%02" PRIx32 " ret=0x%04x",
                 mode,
                 ret);
    }
}

static void schedule_commissioning(uint8_t mode)
{
    esp_err_t err = esp_zigbee_task_queue_post(
        commissioning_start_cb,
        (void *)(uintptr_t)mode
    );

    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "No se pudo postear commissioning mode=0x%02x: %s",
                 mode,
                 esp_err_to_name(err));
    }
}

static bool app_signal_handler(const ezb_app_signal_t *signal)
{
    ezb_app_signal_type_t sig_type = ezb_app_signal_get_type(signal);

    switch (sig_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        schedule_commissioning(EZB_BDB_MODE_INITIALIZATION);
        return true;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
        ESP_LOGI(TAG, "Zigbee End Device first start signal=0x%04x", sig_type);
        ESP_LOGI(TAG, "Start network steering with install code");
        schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        return true;

    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
        ESP_LOGI(TAG, "Zigbee End Device reboot signal=0x%04x", sig_type);

        if (ezb_nwk_get_short_address() != EZB_NWK_ADDR_UNKNOWN) {
            s_joined = true;

            ESP_LOGI(TAG,
                     "Nodo Zigbee ya estaba unido PAN_ID=0x%04hx Channel=%u Short=0x%04hx",
                     ezb_nwk_get_panid(),
                     ezb_nwk_get_current_channel(),
                     ezb_nwk_get_short_address());
        } else {
            s_joined = false;

            ESP_LOGW(TAG, "Reboot sin short address valido, iniciando network steering");
            schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        }

        return true;

    case EZB_BDB_SIGNAL_STEERING:
        if (ezb_nwk_get_short_address() != EZB_NWK_ADDR_UNKNOWN) {
            s_joined = true;

            ESP_LOGI(TAG, "Nodo Zigbee unido a red");
            ESP_LOGI(TAG,
                     "Joined network PAN_ID=0x%04hx Channel=%u Short=0x%04hx",
                     ezb_nwk_get_panid(),
                     ezb_nwk_get_current_channel(),
                     ezb_nwk_get_short_address());
        } else {
            s_joined = false;

            ESP_LOGW(TAG,
                     "Network steering terminado pero sin short address valido, reintentando");

            schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        }
        return true;

    case EZB_ZDO_SIGNAL_LEAVE:
        s_joined = false;
        ESP_LOGW(TAG, "Nodo salio de la red Zigbee");
        return true;

    default:
        ESP_LOGI(TAG,
                 "Zigbee signal=%s (0x%04x)",
                 ezb_app_signal_to_string(sig_type),
                 sig_type);
        return false;
    }
}

static void custom_client_cluster_init(uint8_t ep_id)
{
    ESP_LOGI(TAG,
             "Inicializando custom cluster client endpoint=%u cluster=0x%04x",
             ep_id,
             ZIGBEE_CUSTOM_CLUSTER_ID);

    ezb_zcl_custom_cluster_handlers_t custom_handlers = {
        .cluster_id     = ZIGBEE_CUSTOM_CLUSTER_ID,
        .cluster_role   = EZB_ZCL_CLUSTER_CLIENT,
        .process_cmd_cb = NULL,
        .check_value_cb = NULL,
        .write_attr_cb  = NULL,
        .cmd_disc_cb    = NULL,
    };

    ezb_zcl_custom_cluster_handlers_register(&custom_handlers);
}

static esp_err_t create_custom_client_device(uint8_t ep_id)
{
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();

    ESP_RETURN_ON_FALSE(dev_desc != NULL,
                        ESP_FAIL,
                        TAG,
                        "No se pudo crear device desc");

    ezb_af_ep_desc_t ep_desc = NULL;

    ezb_zcl_cluster_desc_t basic_desc = NULL;
    ezb_zcl_cluster_desc_t identify_desc = NULL;
    ezb_zcl_cluster_desc_t custom_desc = NULL;

    ezb_zcl_basic_cluster_server_config_t basic_cfg = {
        .zcl_version = EZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = EZB_ZCL_BASIC_POWER_SOURCE_BATTERY,
    };

    basic_desc = ezb_zcl_basic_create_cluster_desc(
        &basic_cfg,
        EZB_ZCL_CLUSTER_SERVER
    );

    ESP_RETURN_ON_FALSE(basic_desc != NULL,
                        ESP_FAIL,
                        TAG,
                        "No se pudo crear Basic cluster");

    ESP_ERROR_CHECK(ezb_zcl_basic_cluster_desc_add_attr(
        basic_desc,
        EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
        (void *)"\x09" "ESPRESSIF"
    ));

    static uint8_t model_name[] = "\x11" "tesis-zigbee-nodo";

    ESP_ERROR_CHECK(ezb_zcl_basic_cluster_desc_add_attr(
        basic_desc,
        EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
        model_name
    ));

    ezb_zcl_identify_cluster_server_config_t identify_cfg = {
        .identify_time = EZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };

    identify_desc = ezb_zcl_identify_create_cluster_desc(
        &identify_cfg,
        EZB_ZCL_CLUSTER_SERVER
    );

    ESP_RETURN_ON_FALSE(identify_desc != NULL,
                        ESP_FAIL,
                        TAG,
                        "No se pudo crear Identify cluster");

    ezb_zcl_custom_cluster_config_t custom_cfg = {
        .cluster_id = ZIGBEE_CUSTOM_CLUSTER_ID,
        .init_func = custom_client_cluster_init,
        .deinit_func = NULL,
    };

    custom_desc = ezb_zcl_custom_create_cluster_desc(
        &custom_cfg,
        EZB_ZCL_CLUSTER_CLIENT
    );

    ESP_RETURN_ON_FALSE(custom_desc != NULL,
                        ESP_FAIL,
                        TAG,
                        "No se pudo crear Custom client cluster");

    ezb_af_ep_config_t ep_config = {
        .ep_id = ep_id,
        .app_profile_id = EZB_AF_HA_PROFILE_ID,
        .app_device_id = ZIGBEE_CUSTOM_DEVICE_ID,
        .app_device_version = 0,
    };

    ep_desc = ezb_af_create_endpoint_desc(&ep_config);

    ESP_RETURN_ON_FALSE(ep_desc != NULL,
                        ESP_FAIL,
                        TAG,
                        "No se pudo crear endpoint desc");

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

static void zigbee_task(void *pv)
{
    (void)pv;

    ESP_LOGI(TAG, "Iniciando Zigbee End Device con Install Code");

    esp_zigbee_config_t zb_cfg = {
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE,
            .install_code_policy = true,
            .zed_config = {
                .ed_timeout = 64,
                .keep_alive = ZIGBEE_ED_KEEPALIVE_MS,
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

    ESP_LOGI(TAG, "Configurando Install Code del nodo despues de esp_zigbee_init");

    ESP_ERROR_CHECK(install_code_joiner_set_128(ZIGBEE_INSTALL_CODE_HEX));

    ESP_LOGI(TAG, "Install Code configurado correctamente en el nodo");

    ezb_set_tx_power(ZIGBEE_TX_POWER_DBM);

    int8_t tx_power_dbm = 0;
    ezb_get_tx_power(&tx_power_dbm);
    ESP_LOGI(TAG, "Potencia TX Zigbee configurada=%d dBm", tx_power_dbm);

    ezb_nwk_set_rx_on_when_idle(false);

    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(
        ezb_set_channel_mask(ZIGBEE_PRIMARY_CHANNEL_MASK)
    ));

    ESP_ERROR_CHECK(create_custom_client_device(ZIGBEE_SENSOR_ENDPOINT));

    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(
        ezb_app_signal_add_handler(app_signal_handler)
    ));

    ESP_LOGI(TAG,
             "Nodo Zigbee configurado endpoint=%u PAN_ID objetivo=0x%04x channel=%d",
             ZIGBEE_SENSOR_ENDPOINT,
             ZIGBEE_PAN_ID,
             ZIGBEE_CHANNEL);

    ESP_ERROR_CHECK(esp_zigbee_start(false));

    ESP_ERROR_CHECK(esp_zigbee_launch_mainloop());

    ESP_ERROR_CHECK(esp_zigbee_deinit());

    vTaskDelete(NULL);
}

esp_err_t zigbee_sender_start(void)
{
    BaseType_t ok = xTaskCreate(
        zigbee_task,
        "Zigbee_main",
        8192,
        NULL,
        5,
        NULL
    );

    ESP_RETURN_ON_FALSE(ok == pdPASS,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "No se pudo crear tarea Zigbee");

    return ESP_OK;
}

bool zigbee_sender_is_joined(void)
{
    return s_joined;
}

esp_err_t zigbee_sender_send_packet(const secure_zigbee_packet_t *packet)
{
    ESP_RETURN_ON_FALSE(packet != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "packet NULL");

    ESP_RETURN_ON_FALSE(s_joined,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "Nodo aun no unido a red Zigbee");

    ezb_zcl_custom_cluster_cmd_t cmd_req = {
        .cmd_ctrl = {
            .dst_addr = {
                .addr_mode = EZB_ADDR_MODE_SHORT,
                .u.short_addr = 0x0000,
            },
            .src_ep = ZIGBEE_SENSOR_ENDPOINT,
            .dst_ep = ZIGBEE_GATEWAY_ENDPOINT,
            .cluster_id = ZIGBEE_CUSTOM_CLUSTER_ID,
        },
        .cmd_id = ZIGBEE_TELEMETRY_CMD_ID,
        .data_length = sizeof(secure_zigbee_packet_t),
        .data = (void *)packet,
    };

    ESP_LOGI(TAG,
             "Enviando paquete seguro Zigbee batch=%u len=%u encrypted_len=%u",
             (unsigned)packet->batch_id,
             (unsigned)sizeof(secure_zigbee_packet_t),
             (unsigned)packet->encrypted_len);

    if (!esp_zigbee_lock_acquire(portMAX_DELAY)) {
        ESP_LOGE(TAG, "No se pudo adquirir lock Zigbee");
        return ESP_ERR_TIMEOUT;
    }

    ezb_err_t zb_ret = ezb_zcl_custom_cluster_cmd_req(&cmd_req);

    esp_zigbee_lock_release();

    esp_err_t err = esp_zigbee_err_to_esp(zb_ret);

    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "Paquete seguro Zigbee enviado OK batch=%u",
                 (unsigned)packet->batch_id);
    } else {
        ESP_LOGE(TAG,
                 "Error envio Zigbee batch=%u: %s",
                 (unsigned)packet->batch_id,
                 esp_err_to_name(err));
    }

    return err;
}
void zigbee_sender_deinit_before_sleep(void)
{
    ESP_LOGI(TAG, "Zigbee listo para deep sleep");
}