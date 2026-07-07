#ifndef CONFIG_H
#define CONFIG_H

#define NODE_ID "N1"
#define ESPNOW_CHANNEL 8
#define STREAM_SAMPLE_COUNT 1
#define STREAM_SAMPLE_DELAY_MS 10
#define RTC_WAKEUP_TIME_US (5ULL * 1000000ULL)
#define WIFI_TX_POWER_QDBM 20

/* Sustituir por las MAC STA reales antes de flashear. */
#define ROUTER_MAC_BYTES  {0x40,0x4C,0xCA,0x4D,0x03,0x00}
#define ESPNOW_PMK_BYTES  { 'P','M','K','_','E','S','P','N','O','W','_','2','0','2','6','!' }
#define ESPNOW_LMK_BYTES  { 'L','M','K','_','N','O','D','O','_','G','A','T','E','_','0','1' }

#endif
