#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
#include "openthread/platform/radio.h"
#include "config.h"
#include "thread_sender.h"

static TaskHandle_t s_ot_task = NULL;
static bool s_ot_started = false;
static const char *OT_TAG = "thread";

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

#if NODE_DEBUG_LOGS
    ESP_LOGI(OT_TAG, "Dataset Thread aplicado correctamente");
#endif

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

    ESP_RETURN_ON_ERROR(esp_openthread_start(&config), OT_TAG, "esp_openthread_start");

    xTaskCreate(ot_task, "ot_mainloop", 12288, NULL, 5, &s_ot_task);

    //vTaskDelay(pdMS_TO_TICKS(100));

    ESP_RETURN_ON_ERROR(build_and_set_dataset(), OT_TAG, "dataset");
    //vTaskDelay(pdMS_TO_TICKS(100));

    otInstance *ins = esp_openthread_get_instance();
    if (!ins) {
        ESP_LOGE(OT_TAG, "OpenThread instance NULL");
        return ESP_FAIL;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    otError pwr_err = otPlatRadioSetTransmitPower(ins, THREAD_TX_POWER_DBM);
    if (pwr_err != OT_ERROR_NONE) {
        ESP_LOGW(OT_TAG, "No se pudo configurar potencia TX Thread=%d dBm, err=%d",
                THREAD_TX_POWER_DBM, pwr_err);
    } else {
        int8_t tx_power_dbm = 0;
        otPlatRadioGetTransmitPower(ins, &tx_power_dbm);
        ESP_LOGI(OT_TAG, "Potencia TX Thread configurada=%d dBm", tx_power_dbm);
    }

    otError ot_err = otIp6SetEnabled(ins, true);
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

#if NODE_DEBUG_LOGS
    ESP_LOGI(OT_TAG, "OpenThread iniciado, IPv6 y Thread habilitados");
#endif

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
#if NODE_DEBUG_LOGS
                ESP_LOGI(OT_TAG, "Thread attached role=%d", role);
#endif
                return ESP_OK;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
    }

    ESP_LOGW(OT_TAG, "Timeout esperando attach Thread");
    return ESP_ERR_TIMEOUT;
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

esp_err_t thread_sender_init(void)
{
    ESP_RETURN_ON_ERROR(thread_stack_start_common(), OT_TAG, "start");
    return wait_thread_attached(THREAD_ATTACH_TIMEOUT_MS);
}

esp_err_t thread_sender_send_packet(const thread_app_packet_t *packet)
{
    if (!packet) {
        return ESP_ERR_INVALID_ARG;
    }

    int sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(OT_TAG, "socket UDP error");
        return ESP_FAIL;
    }

    struct timeval tv = {
        .tv_sec = THREAD_UDP_SEND_TIMEOUT_MS / 1000,
        .tv_usec = (THREAD_UDP_SEND_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in6 dest = {0};
    dest.sin6_family = AF_INET6;
    dest.sin6_port = htons(THREAD_UDP_PORT);

    if (inet_pton(AF_INET6, THREAD_GATEWAY_IPV6_ADDR, &dest.sin6_addr) != 1) {
        ESP_LOGE(OT_TAG, "IPv6 gateway invalida: %s", THREAD_GATEWAY_IPV6_ADDR);
        close(sock);
        return ESP_ERR_INVALID_ARG;
    }

    ssize_t sent = sendto(
        sock,
        packet,
        sizeof(*packet),
        0,
        (struct sockaddr *)&dest,
        sizeof(dest)
    );

    close(sock);

    if (sent != sizeof(*packet)) {
        ESP_LOGE(OT_TAG, "sendto fallo sent=%d esperado=%u", (int)sent, (unsigned)sizeof(*packet));
        return ESP_FAIL;
    }

#if NODE_DEBUG_LOGS
    ESP_LOGI(OT_TAG, "Paquete UDP Thread enviado a %s", THREAD_GATEWAY_IPV6_ADDR);
#endif

    return ESP_OK;
}

esp_err_t thread_sender_stop(void)
{
    return thread_stack_stop_common();
}
