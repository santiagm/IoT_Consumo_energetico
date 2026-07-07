#ifndef THREAD_SENDER_H
#define THREAD_SENDER_H
#include "esp_err.h"
#include "thread_packet.h"
esp_err_t thread_sender_init(void);
esp_err_t thread_sender_send_packet(const thread_app_packet_t *packet);
esp_err_t thread_sender_stop(void);
#endif
