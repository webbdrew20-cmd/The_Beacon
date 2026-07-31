#pragma once
#include "esp_err.h"
#include <stdint.h>

typedef struct {
    uint32_t id;
    char     dir;        
    char     from[40];   
    int      ch;        
    char     text[240];
    int64_t  t_ms;      
} beacon_msg_t;

esp_err_t msg_log_init(void);
uint32_t  msg_log_append(char dir, const char *from, int ch, const char *text);
int       msg_log_since(uint32_t since_id, beacon_msg_t *out, int max);
