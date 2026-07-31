#include "esp_log.h"
#include "nvs_flash.h"
#include "wroom2_config.h"
#include "sd_spi.h"
#include "wifi_sta.h"
#include "uart_links2.h"
#include "http_media.h"

static const char *TAG = "wroom2";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    if (sd_spi_init() != ESP_OK) {
        ESP_LOGE(TAG, "Halting: no SD card. Fix wiring/pins and reboot.");
        return;
    }
    ESP_ERROR_CHECK(wifi_sta_start());
    ESP_ERROR_CHECK(uart_links2_init());
    ESP_ERROR_CHECK(http_media_start());

    ESP_LOGI(TAG, "Entertainment board up — music at http://%s/music/", W2_IP);
}
