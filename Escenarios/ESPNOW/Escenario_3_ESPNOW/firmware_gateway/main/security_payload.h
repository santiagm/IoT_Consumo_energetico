#ifndef SECURITY_PAYLOAD_H
#define SECURITY_PAYLOAD_H

#include <stdint.h>

/*
 * Seguridad de aplicacion para el Escenario 4.
 * Esta clave AES-128-GCM debe coincidir exactamente en nodo y gateway.
 * No reemplaza la seguridad nativa ESP-NOW; se aplica sobre sensor_packet_t
 * antes de entregar el buffer a esp_now_send().
 */
#define APP_AES_GCM_KEY_BYTES   { 'A','E','S','G','C','M','_','E','S','P','N','O','W','2','6','!' }

#define ESPNOW_APP_SEC_MAGIC    0x474E5345UL  /* "ESNG" en little-endian */
#define ESPNOW_APP_SEC_VERSION  1U
#define AES_GCM_IV_LEN          12U
#define AES_GCM_TAG_LEN         16U

#endif /* SECURITY_PAYLOAD_H */
