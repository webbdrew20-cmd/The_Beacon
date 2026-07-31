idf_component_register(
    SRCS
        "wroom2_main.c"
        "sd_spi.c"
        "wifi_sta.c"
        "uart_links2.c"
        "http_media.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi esp_netif nvs_flash esp_http_server
        fatfs sdmmc json driver vfs esp_timer
)
