#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "ble_gatt_relay.h"

static const char *TAG = "BLE_RELAY_N3_MAIN";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_LOGI(TAG, "Iniciando N3 relay BLE GATT seguro. Malla logica N1/N2 -> N3 -> Gateway");
    ESP_ERROR_CHECK(ble_gatt_relay_init());
}
