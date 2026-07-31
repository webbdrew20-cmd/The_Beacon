#pragma once
#include "esp_err.h"

esp_err_t sd_mount_init(void);


void sd_lock(void);
void sd_unlock(void);
