#include "dns_server.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "dns";
static uint32_t s_ip_be;   

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS sinkhole up on :53");

    uint8_t buf[600];
    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0,
                           (struct sockaddr *)&from, &fromlen);
        if (len < 12) continue;

        
        int i = 12;
        while (i < len && buf[i] != 0) i += buf[i] + 1;
        if (i + 5 > len) continue;         
        i++;                               
        uint16_t qtype  = (buf[i] << 8) | buf[i + 1];
        uint16_t qclass = (buf[i + 2] << 8) | buf[i + 3];
        int qend = i + 4;

        
        buf[2] = 0x81; buf[3] = 0x80;
        buf[6] = 0; buf[7] = 0;            
        buf[8] = 0; buf[9] = 0;           
        buf[10] = 0; buf[11] = 0;         
        buf[4] = 0; buf[5] = 1;            

        int rlen = qend;
        if ((qtype == 1 /*A*/ || qtype == 255 /*ANY*/) && qclass == 1) {
            buf[7] = 1;                   
            uint8_t *p = buf + qend;
            *p++ = 0xC0; *p++ = 0x0C;      
            *p++ = 0x00; *p++ = 0x01;      
            *p++ = 0x00; *p++ = 0x01;      
            *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 60;  
            *p++ = 0x00; *p++ = 0x04;      
            memcpy(p, &s_ip_be, 4); p += 4;
            rlen = p - buf;
        }
        sendto(sock, buf, rlen, 0, (struct sockaddr *)&from, fromlen);
    }
}

esp_err_t dns_server_start(uint32_t resolve_ip_be)
{
    s_ip_be = resolve_ip_be;
    return (xTaskCreate(dns_task, "dns", 4096, NULL, 5, NULL) == pdPASS)
               ? ESP_OK : ESP_FAIL;
}
