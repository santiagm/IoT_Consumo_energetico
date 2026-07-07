#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_err.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_vfs_eventfd.h"

#include "esp_openthread.h"
#include "esp_openthread_types.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"

#include "openthread/thread.h"
#include "openthread/dataset.h"
#include "openthread/ip6.h"
#include "openthread/udp.h"
#include "openthread/message.h"

#include "config.h"
#include "thread_receiver.h"


static TaskHandle_t s_ot_task = NULL;
static bool s_ot_started = false;
static const char *OT_TAG = "thread";

static thread_packet_cb_t s_cb = NULL;
static void *s_cb_ctx = NULL;

typedef struct {
    thread_app_packet_t packet;
    int8_t rssi;
    int16_t lqi;
} thread_rx_event_t;

static QueueHandle_t s_rx_queue = NULL;
static otUdpSocket s_udp_socket;
static bool s_udp_opened = false;


static void ot_task(void *arg)
{
    (void)arg;

    esp_openthread_launch_mainloop();

    vTaskDelete(NULL);
}


static esp_err_t build_and_set_dataset(void)
{
    otOperationalDataset dataset;
    memset(&dataset, 0, sizeof(dataset));

    dataset.mActiveTimestamp.mSeconds = 1;
    dataset.mComponents.mIsActiveTimestampPresent = true;

    dataset.mChannel = THREAD_CHANNEL;
    dataset.mComponents.mIsChannelPresent = true;

    dataset.mPanId = THREAD_PANID;
    dataset.mComponents.mIsPanIdPresent = true;

    memcpy(dataset.mExtendedPanId.m8, THREAD_EXT_PANID, sizeof(THREAD_EXT_PANID));
    dataset.mComponents.mIsExtendedPanIdPresent = true;

    memcpy(dataset.mNetworkKey.m8, THREAD_NETWORK_KEY, sizeof(THREAD_NETWORK_KEY));
    dataset.mComponents.mIsNetworkKeyPresent = true;

    memcpy(dataset.mMeshLocalPrefix.m8, THREAD_MESH_LOCAL_PREFIX, sizeof(THREAD_MESH_LOCAL_PREFIX));
    dataset.mComponents.mIsMeshLocalPrefixPresent = true;

    memcpy(dataset.mPskc.m8, THREAD_PSKC, sizeof(THREAD_PSKC));
    dataset.mComponents.mIsPskcPresent = true;

    size_t n = strlen(THREAD_NETWORK_NAME);

    if (n > OT_NETWORK_NAME_MAX_SIZE) {
        ESP_LOGE(OT_TAG, "Nombre de red Thread demasiado largo");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(dataset.mNetworkName.m8, THREAD_NETWORK_NAME, n + 1);
    dataset.mComponents.mIsNetworkNamePresent = true;

    otInstance *ins = esp_openthread_get_instance();

    if (!ins) {
        ESP_LOGE(OT_TAG, "OpenThread instance NULL al aplicar dataset");
        return ESP_FAIL;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);

    otError e = otDatasetSetActive(ins, &dataset);

    esp_openthread_lock_release();

    if (e != OT_ERROR_NONE) {
        ESP_LOGE(OT_TAG, "otDatasetSetActive fallo: %d", e);
        return ESP_FAIL;
    }

    ESP_LOGI(OT_TAG, "Dataset Thread aplicado correctamente");

    return ESP_OK;
}


static esp_err_t thread_stack_start_common(void)
{
    if (s_ot_started) {
        return ESP_OK;
    }

    esp_err_t ret;

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(OT_TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(OT_TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 3,
    };

    ret = esp_vfs_eventfd_register(&eventfd_config);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(OT_TAG, "esp_vfs_eventfd_register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_openthread_config_t config = {
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
        .platform_config = {
            .radio_config = {
                .radio_mode = RADIO_MODE_NATIVE,
            },
            .host_config = {
                .host_connection_mode = HOST_CONNECTION_MODE_NONE,
            },
            .port_config = {
                .storage_partition_name = "nvs",
                .netif_queue_size = 10,
                .task_queue_size = 10,
            },
        },
    };

    ESP_RETURN_ON_ERROR(
        esp_openthread_start(&config),
        OT_TAG,
        "esp_openthread_start"
    );

    xTaskCreate(
        ot_task,
        "ot_mainloop",
        12288,
        NULL,
        5,
        &s_ot_task
    );

    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_RETURN_ON_ERROR(
        build_and_set_dataset(),
        OT_TAG,
        "dataset"
    );

    otInstance *ins = esp_openthread_get_instance();

    if (!ins) {
        ESP_LOGE(OT_TAG, "OpenThread instance NULL");
        return ESP_FAIL;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);

    otError ot_err;

    ot_err = otIp6SetEnabled(ins, true);
    if (ot_err != OT_ERROR_NONE) {
        esp_openthread_lock_release();
        ESP_LOGE(OT_TAG, "otIp6SetEnabled failed: %d", ot_err);
        return ESP_FAIL;
    }

    ot_err = otThreadSetEnabled(ins, true);
    if (ot_err != OT_ERROR_NONE) {
        esp_openthread_lock_release();
        ESP_LOGE(OT_TAG, "otThreadSetEnabled failed: %d", ot_err);
        return ESP_FAIL;
    }

    esp_openthread_lock_release();

    s_ot_started = true;

    ESP_LOGI(OT_TAG, "OpenThread iniciado, IPv6 y Thread habilitados");

    return ESP_OK;
}


static esp_err_t wait_thread_attached(uint32_t timeout_ms)
{
    uint32_t elapsed = 0;

    while (elapsed < timeout_ms) {
        otInstance *ins = esp_openthread_get_instance();

        if (ins) {
            esp_openthread_lock_acquire(portMAX_DELAY);

            otDeviceRole role = otThreadGetDeviceRole(ins);

            esp_openthread_lock_release();

            if (role == OT_DEVICE_ROLE_CHILD ||
                role == OT_DEVICE_ROLE_ROUTER ||
                role == OT_DEVICE_ROLE_LEADER) {
                ESP_LOGI(OT_TAG, "Thread attached role=%d", role);
                return ESP_OK;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(250));
        elapsed += 250;
    }

    ESP_LOGW(OT_TAG, "Timeout esperando attach Thread");

    return ESP_ERR_TIMEOUT;
}


static void print_thread_ipv6_addresses(void)
{
    otInstance *ins = esp_openthread_get_instance();

    if (!ins) {
        ESP_LOGW(OT_TAG, "No se pudo obtener instancia OpenThread para imprimir IPv6");
        return;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);

    const otNetifAddress *addr = otIp6GetUnicastAddresses(ins);

    while (addr != NULL) {
        char ipv6_str[OT_IP6_ADDRESS_STRING_SIZE];

        otIp6AddressToString(
            &addr->mAddress,
            ipv6_str,
            sizeof(ipv6_str)
        );

        ESP_LOGI(
            OT_TAG,
            "Gateway Thread IPv6: %s / prefix=%u",
            ipv6_str,
            addr->mPrefixLength
        );

        addr = addr->mNext;
    }

    esp_openthread_lock_release();
}


static esp_err_t thread_stack_stop_common(void)
{
    if (!s_ot_started) {
        return ESP_OK;
    }

    esp_openthread_mainloop_exit();

    vTaskDelay(pdMS_TO_TICKS(200));

    esp_openthread_stop();

    s_ot_started = false;

    return ESP_OK;
}


static bool thread_ipv6_addr_to_rloc16(const otIp6Address *addr, uint16_t *rloc16)
{
    if (addr == NULL || rloc16 == NULL) {
        return false;
    }

    const uint8_t *a = addr->mFields.m8;

    /*
     * Thread RLOC IPv6 IID format:
     * xxxx:xxxx:xxxx:xxxx:0000:00ff:fe00:RLOC16
     */
    if (a[8] == 0x00 &&
        a[9] == 0x00 &&
        a[10] == 0x00 &&
        a[11] == 0xff &&
        a[12] == 0xfe &&
        a[13] == 0x00) {
        *rloc16 = ((uint16_t)a[14] << 8) | a[15];
        return true;
    }

    return false;
}

static bool get_thread_neighbor_metrics(const otMessageInfo *message_info,
                                        int8_t *neighbor_rssi,
                                        int16_t *neighbor_lqi)
{
    if (neighbor_rssi != NULL) {
        *neighbor_rssi = 127;
    }

    if (neighbor_lqi != NULL) {
        *neighbor_lqi = -1;
    }

    otInstance *ins = esp_openthread_get_instance();
    if (ins == NULL) {
        ESP_LOGW(OT_TAG, "No se pudo obtener instancia OpenThread para neighbor metrics");
        return false;
    }

    bool has_peer_rloc = false;
    uint16_t peer_rloc16 = 0;

    if (message_info != NULL) {
        has_peer_rloc = thread_ipv6_addr_to_rloc16(&message_info->mPeerAddr, &peer_rloc16);
    }

    otNeighborInfoIterator iterator = OT_NEIGHBOR_INFO_ITERATOR_INIT;
    otNeighborInfo nbr;
    otNeighborInfo best_nbr;
    bool found_match = false;
    bool found_any = false;

    memset(&best_nbr, 0, sizeof(best_nbr));

    while (otThreadGetNextNeighborInfo(ins, &iterator, &nbr) == OT_ERROR_NONE) {
        ESP_LOGI(OT_TAG,
                 "DEBUG_THREAD_NEIGHBOR: rloc16=0x%04x LQ_IN=%u avg_rssi=%d last_rssi=%d age=%lu child=%d rx_on_idle=%d",
                 nbr.mRloc16,
                 nbr.mLinkQualityIn,
                 nbr.mAverageRssi,
                 nbr.mLastRssi,
                 (unsigned long)nbr.mAge,
                 nbr.mIsChild,
                 nbr.mRxOnWhenIdle);

        if (!found_any || nbr.mAge < best_nbr.mAge) {
            best_nbr = nbr;
            found_any = true;
        }

        if (has_peer_rloc && nbr.mRloc16 == peer_rloc16) {
            best_nbr = nbr;
            found_match = true;
            break;
        }
    }

    if (!found_match && !found_any) {
        ESP_LOGW(OT_TAG, "No hay vecinos Thread en tabla para obtener RSSI/LQI");
        return false;
    }

    int8_t selected_rssi = best_nbr.mLastRssi;

    if (selected_rssi == 127) {
        selected_rssi = best_nbr.mAverageRssi;
    }

    if (neighbor_rssi != NULL) {
        *neighbor_rssi = selected_rssi;
    }

    if (neighbor_lqi != NULL) {
        /*
         * OpenThread maneja Link Quality In en escala pequena.
         * Normalmente 0 = malo/no usable y 3 = enlace bueno.
         */
        *neighbor_lqi = (int16_t)best_nbr.mLinkQualityIn;
    }

    ESP_LOGI(OT_TAG,
             "DEBUG_THREAD_NEIGHBOR_MATCH: %s peer_rloc16=0x%04x used_rloc16=0x%04x RSSI=%d LQ_IN=%u avg_rssi=%d last_rssi=%d age=%lu",
             found_match ? "MATCH" : "FALLBACK_RECENT",
             has_peer_rloc ? peer_rloc16 : 0xffff,
             best_nbr.mRloc16,
             selected_rssi,
             best_nbr.mLinkQualityIn,
             best_nbr.mAverageRssi,
             best_nbr.mLastRssi,
             (unsigned long)best_nbr.mAge);

    return true;
}



static void udp_receive_callback(void *context, otMessage *message, const otMessageInfo *message_info)
{
    (void)context;
    (void)message_info;

    if (!message || !s_rx_queue) {
        return;
    }

    thread_rx_event_t ev = {0};

    uint16_t payload_offset = otMessageGetOffset(message);
    uint16_t payload_len = otMessageGetLength(message) - payload_offset;

    if (payload_len != sizeof(thread_app_packet_t)) {
        ESP_LOGW(OT_TAG, "paquete UDP inválido len=%u esperado=%u",
                 payload_len, (unsigned)sizeof(thread_app_packet_t));
        return;
    }

    int read_len = otMessageRead(message,
                                 payload_offset,
                                 &ev.packet,
                                 sizeof(ev.packet));
    if (read_len != sizeof(thread_app_packet_t)) {
        ESP_LOGW(OT_TAG, "otMessageRead incompleto len=%d", read_len);
        return;
    }

    /*
     * Primero tomamos el RSSI/LQI desde la tabla de vecinos Thread.
     * En OpenThread, LinkQualityIn usa escala pequena: 0..3.
     * Esto es equivalente al enfoque de tabla de vecinos que usamos en Zigbee,
     * pero con la escala propia de Thread.
     */
    ev.rssi = otMessageGetRss(message);
    ev.lqi = -1;

    int8_t neighbor_rssi = 127;
    int16_t neighbor_lqi = -1;

    if (get_thread_neighbor_metrics(message_info, &neighbor_rssi, &neighbor_lqi)) {
        if (neighbor_rssi != 127) {
            ev.rssi = neighbor_rssi;
        }
        ev.lqi = neighbor_lqi;
    } else {
        /* Respaldo: información cruda del paquete recibido. */
        otThreadLinkInfo link_info;
        memset(&link_info, 0, sizeof(link_info));

        otError link_err = otMessageGetThreadLinkInfo(message, &link_info);

        if (link_err == OT_ERROR_NONE) {
            ev.rssi = link_info.mRss;
            ev.lqi = (int16_t)link_info.mLqi;

            ESP_LOGI(OT_TAG,
                     "DEBUG_THREAD_RAW_FALLBACK: RSSI=%d dBm, RAW_LQI=%u, CH=%u, SEC=%d, RADIO=%u",
                     link_info.mRss,
                     link_info.mLqi,
                     link_info.mChannel,
                     link_info.mLinkSecurity,
                     link_info.mRadioType);
        } else {
            ESP_LOGW(OT_TAG,
                     "No se pudo obtener Thread neighbor ni LinkInfo. err=%d, RSSI=%d, LQI=-1",
                     link_err,
                     ev.rssi);
        }
    }

    if (xQueueSend(s_rx_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(OT_TAG, "cola RX llena, paquete descartado");
    }
}


static esp_err_t open_thread_udp_socket(void)
{
    otInstance *ins = esp_openthread_get_instance();
    if (!ins) {
        return ESP_FAIL;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);

    otError err = OT_ERROR_NONE;

    if (!s_udp_opened) {
        memset(&s_udp_socket, 0, sizeof(s_udp_socket));
        err = otUdpOpen(ins, &s_udp_socket, udp_receive_callback, NULL);
        if (err != OT_ERROR_NONE) {
            esp_openthread_lock_release();
            ESP_LOGE(OT_TAG, "otUdpOpen fallo: %d", err);
            return ESP_FAIL;
        }
        s_udp_opened = true;
    }

    otSockAddr listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.mPort = THREAD_UDP_PORT;

    err = otUdpBind(ins, &s_udp_socket, &listen_addr, OT_NETIF_THREAD_HOST);

    esp_openthread_lock_release();

    if (err != OT_ERROR_NONE) {
        ESP_LOGE(OT_TAG, "otUdpBind fallo: %d", err);
        return ESP_FAIL;
    }

    ESP_LOGI(OT_TAG, "OpenThread UDP callback escuchando puerto %d", THREAD_UDP_PORT);
    return ESP_OK;
}


static void receiver_task(void *arg)
{
    (void)arg;

    thread_rx_event_t ev;

    while (1) {
        if (xQueueReceive(s_rx_queue, &ev, portMAX_DELAY) == pdTRUE) {
            if (s_cb) {
                s_cb(&ev.packet, ev.rssi, ev.lqi, s_cb_ctx);
            }
        }
    }
}


esp_err_t thread_receiver_init(void)
{
    ESP_RETURN_ON_ERROR(
        thread_stack_start_common(),
        OT_TAG,
        "start"
    );

    ESP_RETURN_ON_ERROR(
        wait_thread_attached(THREAD_ATTACH_TIMEOUT_MS),
        OT_TAG,
        "wait attached"
    );

    print_thread_ipv6_addresses();

    return ESP_OK;
}


esp_err_t thread_receiver_start(thread_packet_cb_t cb, void *ctx)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cb = cb;
    s_cb_ctx = ctx;

    if (!s_rx_queue) {
        s_rx_queue = xQueueCreate(10, sizeof(thread_rx_event_t));
        if (!s_rx_queue) {
            ESP_LOGE(OT_TAG, "No se pudo crear cola RX Thread");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_RETURN_ON_ERROR(
        open_thread_udp_socket(),
        OT_TAG,
        "open udp callback"
    );

    xTaskCreate(
        receiver_task,
        "thread_udp_rx",
        6144,
        NULL,
        5,
        NULL
    );

    return ESP_OK;
}