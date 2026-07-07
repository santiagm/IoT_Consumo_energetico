#ifndef ESPNOW_HANDLER_H
#define ESPNOW_HANDLER_H
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "mesh_payload.h"
typedef struct {
    espnow_mesh_frame_t frame;
    uint8_t sender_mac[6];
    int8_t rssi;
} router_rx_item_t;
esp_err_t espnow_handler_init(QueueHandle_t forward_queue);
esp_err_t espnow_handler_send_gateway(const espnow_mesh_frame_t *frame);
#endif
