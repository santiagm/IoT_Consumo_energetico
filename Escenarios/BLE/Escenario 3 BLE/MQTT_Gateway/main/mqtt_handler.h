#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H
#include <stdbool.h>
#include "esp_err.h"
esp_err_t mqtt_app_start(void);
bool mqtt_is_connected(void);
esp_err_t mqtt_publish_telemetry(const char *json);
#endif
