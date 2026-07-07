#include "zigbee_receiver.h"
#include "uart_bridge_sender.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "GW_INSTALL_CODE";

static void init_nvs_all(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);

    /*
     * Zigbee usa una particion NVS separada llamada zb_storage.
     * Esto evita el error anterior:
     * abort() at ezb_plat_datasets_init
     */
    err = nvs_flash_init_partition("zb_storage");

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition("zb_storage"));
        err = nvs_flash_init_partition("zb_storage");
    }

    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando Zigbee Gateway UART Install Code");

    init_nvs_all();

    ESP_ERROR_CHECK(uart_bridge_sender_init());

    /*
     * IMPORTANTE:
     * No configurar Install Code aqui.
     * Primero debe inicializarse Zigbee dentro de zigbee_receiver_start().
     */
    ESP_ERROR_CHECK(zigbee_receiver_start());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}