#pragma once

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

/* Completa tus credenciales antes de compilar. */
#define WIFI_SSID               "diqueerespobre"
#define WIFI_PASS               "soypobre"

#define THINGSBOARD_HOST        "mqtt.thingsboard.cloud"
#define THINGSBOARD_PORT        1883
#define THINGSBOARD_TOKEN       "Wj2rLAd4xKoOlPpuN0u7"
#define THINGSBOARD_TOPIC       "v1/devices/me/telemetry"
#define MQTT_CLIENT_ID          "gateway-espnow-seguro-esc4"

#define ESPNOW_CHANNEL          6//8 
#define STREAM_SAMPLE_COUNT     1
#define SYNTHETIC_SAMPLE_DELAY_MS 10
#define SNTP_SERVER             "pool.ntp.org"

/* MAC STA real del nodo sensor. Cambia este valor si tu nodo usa otra MAC. */
#define NODE_PEER_MAC_BYTES     {0x98, 0xA3, 0x16, 0x96, 0x8A, 0x50}

