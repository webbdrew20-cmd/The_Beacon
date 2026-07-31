idf_component_register(
    SRCS
        "beacon_main.c"
        "sd_mount.c"
        "dns_server.c"
        "msg_log.c"
        "uart_links.c"
        "zim_reader.c"
        "http_portal.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi esp_netif nvs_flash esp_http_server
        fatfs sdmmc json driver vfs esp_timer zstd
)
