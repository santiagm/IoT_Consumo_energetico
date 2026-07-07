#ifndef CONFIG_H
#define CONFIG_H
#define NODE_ID "N3"
#define ESPNOW_CHANNEL 8
#define STREAM_SAMPLE_COUNT 1
#define STREAM_SAMPLE_DELAY_MS 10
#define ROUTER_SEND_PERIOD_MS 5000
#define WIFI_TX_POWER_QDBM 20
/* Sustituir cada valor por la MAC STA del equipo indicado. */
#define N1_MAC_BYTES      {0x98,0xA3,0x16,0x96,0x8A,0x50}//0x98,0xA3,0x16,0x96,0x8A,0x50
#define N2_MAC_BYTES      {0x40,0x4C,0xCA,0x4E,0x3A,0x34} //0x40,0x4C,0xCA,0x4E,0x3A,0x34
#define GATEWAY_MAC_BYTES {0x40,0x4C,0xCA,0x55,0xAB,0x10}
#define ESPNOW_PMK_BYTES  { 'P','M','K','_','E','S','P','N','O','W','_','2','0','2','6','!' }
#define ESPNOW_LMK_BYTES  { 'L','M','K','_','N','O','D','O','_','G','A','T','E','_','0','1' }
#endif
