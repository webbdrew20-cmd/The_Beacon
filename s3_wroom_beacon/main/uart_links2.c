#include "uart_links2.h"
#include "wroom2_config.h"
#include <string.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "uart";

static void hb_task(void *arg)
{
    const char *hb = "{\"type\":\"hb\"}\n";
    for (;;) {
        uart_write_bytes(UART_P4, hb, strlen(hb));
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_MS));
    }
}

static void status_relay_task(void *arg)
{
    static char line[1100];
    int len = 0;
    uint8_t chunk[256];
    for (;;) {
        int got = uart_read_bytes(UART_STATUS, chunk, sizeof(chunk), pdMS_TO_TICKS(200));
        for (int i = 0; i < got; i++) {
            char c = (char)chunk[i];
            if (c == '\n' || c == '\r') {
                if (len > 0) {
                    line[len] = 0;
                    uart_write_bytes(UART_P4, line, len);
                    uart_write_bytes(UART_P4, "\n", 1);
                    ESP_LOGD(TAG, "relayed: %s", line);
                    len = 0;
                }
            } else if (len < (int)sizeof(line) - 1) {
                line[len++] = c;
            } else {
                len = 0;
            }
        }
    }
}

esp_err_t uart_links2_init(void)
{
    uart_config_t uc = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_P4, 2048, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_P4, &uc));
    ESP_ERROR_CHECK(uart_set_pin(UART_P4, UART_P4_TX, UART_P4_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_ERROR_CHECK(uart_driver_install(UART_STATUS, 2048, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_STATUS, &uc));
    ESP_ERROR_CHECK(uart_set_pin(UART_STATUS, UART_STATUS_TX, UART_STATUS_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(hb_task, "hb", 2560, NULL, 4, NULL);
    xTaskCreate(status_relay_task, "status_relay", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "UART links up (P4 tx%d/rx%d, Status tx%d/rx%d)",
             UART_P4_TX, UART_P4_RX, UART_STATUS_TX, UART_STATUS_RX);
    return ESP_OK;
}
