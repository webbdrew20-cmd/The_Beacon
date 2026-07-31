#pragma once
#include "esp_err.h"
#include <stdint.h>


esp_err_t dns_server_start(uint32_t resolve_ip_be);
