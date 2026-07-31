#pragma once
#define BEACON_SSID       "Beacon"
#define BEACON_PASS       ""           
#define W2_IP             "192.168.4.200"
#define W2_GATEWAY        "192.168.4.1"
#define W2_NETMASK        "255.255.255.0"
#define W2_SD_MISO   12
#define W2_SD_MOSI   11
#define W2_SD_SCK    10
#define W2_SD_CS      9
#define W2_SD_FREQ_KHZ  20000
#define SD_MOUNT     "/sdcard"
#define WWW_ROOT     SD_MOUNT "/www"
#define MUSIC_DIR    SD_MOUNT "/music"
#define ROMS_DIR     SD_MOUNT "/games/roms"
#define EJS_DIR      SD_MOUNT "/games/ejs"
#define UART_P4        UART_NUM_1
#define UART_P4_TX     20
#define UART_P4_RX     19
#define UART_STATUS    UART_NUM_2
#define UART_STATUS_TX 18
#define UART_STATUS_RX 17
#define UART_BAUD      115200
#define HEARTBEAT_MS   3000
#define MEDIA_WORKERS      3
#define MEDIA_WORKQ_DEPTH  8
