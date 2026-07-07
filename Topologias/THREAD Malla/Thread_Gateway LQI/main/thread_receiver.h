#ifndef THREAD_RECEIVER_H
#define THREAD_RECEIVER_H
#include "esp_err.h"
#include "thread_packet.h"
typedef void (*thread_packet_cb_t)(const thread_app_packet_t *packet, int rssi, int lqi, void *ctx);
esp_err_t thread_receiver_init(void);
esp_err_t thread_receiver_start(thread_packet_cb_t cb, void *ctx);
#endif
