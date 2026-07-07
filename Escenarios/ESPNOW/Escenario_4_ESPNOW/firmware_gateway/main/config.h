#pragma once

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

/* Completa tus credenciales antes de compilar. */
#define WIFI_SSID               "diqueerespobre"
#define WIFI_PASS               "soypobre"

#define THINGSBOARD_HOST        "mqtt.thingsboard.cloud"
#define THINGSBOARD_PORT        1883
#define THINGSBOARD_TOKEN       "f4DC5mj0gC45Tf5n1Xq0"
#define THINGSBOARD_TOPIC       "v1/devices/me/telemetry"
#define MQTT_CLIENT_ID          "gateway-espnow-seguro-esc4"

#define ESPNOW_CHANNEL          6//8
#define STREAM_SAMPLE_COUNT     1
#define SYNTHETIC_SAMPLE_DELAY_MS 10
#define SNTP_SERVER             "pool.ntp.org"

/* MAC STA real del nodo sensor. Cambia este valor si tu nodo usa otra MAC. */
#define NODE_PEER_MAC_BYTES     {0x98, 0xA3, 0x16, 0x96, 0x8A, 0x50}

/* Deben coincidir exactamente con el nodo. Cada arreglo tiene 16 bytes. */
#define ESPNOW_PMK_BYTES        { 'P','M','K','_','E','S','P','N','O','W','_','2','0','2','6','!' }
#define ESPNOW_LMK_BYTES        { 'L','M','K','_','N','O','D','O','_','G','A','T','E','_','0','1' }
