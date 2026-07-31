#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "beacon_config.h"
#include "sd_mount.h"
#include "dns_server.h"
#include "msg_log.h"
#include "uart_links.h"
#include "zim_reader.h"
#include "http_portal.h"

static const char *TAG = "beacon";

static esp_netif_t *wifi_ap_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();

    

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&icfg));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.ap.ssid, BEACON_AP_SSID, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = strlen(BEACON_AP_SSID);
    wc.ap.channel = BEACON_AP_CHANNEL;
    wc.ap.max_connection = BEACON_AP_MAX_CONN;
    if (strlen(BEACON_AP_PASS) == 0) {
        wc.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
        strlcpy((char *)wc.ap.password, BEACON_AP_PASS, sizeof(wc.ap.password));
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP \"%s\" up on channel %d", BEACON_AP_SSID, BEACON_AP_CHANNEL);
    return ap;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

   
    if (sd_mount_init() != ESP_OK) {
        ESP_LOGE(TAG, "Halting: no SD card. Fix wiring/pins and reboot.");
        return;
    }
    ESP_ERROR_CHECK(msg_log_init());

    
    esp_netif_t *ap = wifi_ap_start();
    esp_netif_ip_info_t ip;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(ap, &ip));
    ESP_ERROR_CHECK(dns_server_start(ip.ip.addr));

   
    ESP_ERROR_CHECK(zim_registry_init());
    ESP_ERROR_CHECK(uart_links_init());
    ESP_ERROR_CHECK(http_portal_start());

    ESP_LOGI(TAG, "Beacon is up — connect to \"%s\" and browse to 192.168.4.1",
             BEACON_AP_SSID);
}
