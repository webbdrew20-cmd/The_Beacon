#pragma once

#define BEACON_AP_SSID      "Beacon"
#define BEACON_AP_PASS      ""        // empty string = open network
#define BEACON_AP_MAX_CONN  10
#define BEACON_AP_CHANNEL   6


#define SD_MOUNT        "/sdcard"
#define WWW_ROOT        SD_MOUNT "/www"
#define CONTENT_ROOT    SD_MOUNT "/content"
#define INFOWEB_DIR     SD_MOUNT "/content/infoweb"
#define MSG_DIR         SD_MOUNT "/messages"
#define INFOWEB_FF_DIR  "0:/content/infoweb"
#define BEACON_SD_MISO  52
#define BEACON_SD_MOSI  51
#define BEACON_SD_SCK   31
#define BEACON_SD_CS    30
#define BEACON_SD_SPI_FREQ_KHZ  20000
#define UART_WROOM2      UART_NUM_1
#define UART_WROOM2_TX   21
#define UART_WROOM2_RX   20
#define UART_LORA        UART_NUM_2
#define UART_LORA_TX     22
#define UART_LORA_RX     23
#define UART_STATUS      UART_NUM_3
#define UART_STATUS_TX   27
#define UART_STATUS_RX   32
#define UART_BAUD        115200
#define LORA_POLL_MS     3000    
#define NODE_OFFLINE_MS  10000    
#define MSG_CHUNK_MAX_BYTES   (10*1024*1024)        
#define MSG_TOTAL_MAX_BYTES   (1024LL*1024*1024)    
#define MSG_RING_CAP          200                   
#define ZIM_MAX_FILES        16
#define ZIM_BLOB_CAP         (24*1024*1024)  
#define ZIM_SEARCH_PER_FILE  8
#define ZIM_SEARCH_SCAN_CAP  300
#define ZIM_WORKERS            3            
#define ZIM_WORKQ_DEPTH        8            
#define ZIM_CCACHE_SLOTS       8            
#define ZIM_CCACHE_BUDGET      (12*1024*1024)
#define ZIM_CCACHE_MAX_CLUSTER (6*1024*1024)
#define ZIM_DKEY_ENTRIES       32768        
#define ZIM_DKEY_LEN           40
