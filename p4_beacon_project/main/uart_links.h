#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t uart_links_init(void);


esp_err_t uart_links_send_lora(const char *to, int ch, const char *text);


esp_err_t uart_links_send_status(const char *line);


void uart_links_get_nodes(bool *wroom2, bool *lora, bool *status);


char *uart_links_get_env(int64_t *age_ms);
