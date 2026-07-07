#ifndef MESH_PAYLOAD_H
#define MESH_PAYLOAD_H

#include <stdint.h>
#include "esp_now.h"
#include "security_payload.h"

#define ESPNOW_MESH_MAGIC 0x4D455348UL
#define ESPNOW_MESH_VERSION 1U
#define ESPNOW_MESH_TYPE_DATA 1U
#define MESH_RSSI_UNKNOWN (-127)

typedef struct __attribute__((packed)) {
    uint32_t mesh_magic;
    uint8_t mesh_version;
    uint8_t mesh_type;
    char origin_node_id[4];
    uint8_t origin_mac[6];
    char relay_node_id[4];
    uint8_t relay_mac[6];
    uint8_t hop_count;
    int8_t first_hop_rssi;
    int8_t last_hop_rssi;
    uint32_t packet_id_shadow;
    uint16_t batch_id_shadow;
    uint16_t secure_len;
    uint8_t secure_payload[sizeof(secure_espnow_packet_t)];
} espnow_mesh_frame_t;

_Static_assert(sizeof(espnow_mesh_frame_t) <= ESP_NOW_MAX_DATA_LEN,
               "espnow_mesh_frame_t exceeds ESP-NOW payload limit");

#endif
