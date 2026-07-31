#include "uart_links.h"
#include "beacon_config.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "msg_log.h"

static const char *TAG = "uart";

enum { ROLE_WROOM2 = 0, ROLE_LORA = 1, ROLE_STATUS = 2 };
static const int s_ports[3] = { UART_WROOM2, UART_LORA, UART_STATUS };

static int64_t s_last_seen[3] = { -1, -1, -1 };
static char   *s_env_json = NULL;     
static int64_t s_env_time = -1;
static SemaphoreHandle_t s_mtx;

static void uart_send_line(int port, const char *line)
{
    uart_write_bytes(port, line, strlen(line));
    uart_write_bytes(port, "\n", 1);
}

static void handle_line(int role, char *line)
{
    int64_t now = esp_timer_get_time() / 1000;
    cJSON *root = cJSON_Parse(line);
    if (!root) return;                 
    s_last_seen[role] = now;

    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type)) {
        
        if ((role == ROLE_STATUS || role == ROLE_WROOM2) &&
            strcmp(type->valuestring, "status") == 0) {
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            free(s_env_json);
            s_env_json = strdup(line);
            s_env_time = now;
            xSemaphoreGive(s_mtx);
            s_last_seen[ROLE_STATUS] = now;
        }
        else if (role == ROLE_LORA && strcmp(type->valuestring, "msgs") == 0) {
            const cJSON *items = cJSON_GetObjectItem(root, "items");
            int n = 0;
            const cJSON *it;
            cJSON_ArrayForEach(it, items) {
                const cJSON *from = cJSON_GetObjectItem(it, "from");
                const cJSON *ch   = cJSON_GetObjectItem(it, "ch");
                const cJSON *text = cJSON_GetObjectItem(it, "text");
                msg_log_append('i',
                    cJSON_IsString(from) ? from->valuestring : "?",
                    cJSON_IsNumber(ch) ? ch->valueint : 0,
                    cJSON_IsString(text) ? text->valuestring : "");
                n++;
            }
            if (n > 0) {
                ESP_LOGI(TAG, "LoRa delivered %d message(s), acking", n);
                uart_send_line(UART_LORA, "{\"cmd\":\"ack\"}");
            }
        }
        
    }
    cJSON_Delete(root);
}

typedef struct { int role; } rx_arg_t;

static void rx_task(void *arg)
{
    int role = ((rx_arg_t *)arg)->role;
    int port = s_ports[role];
    static char linebuf[3][1100];
    int len = 0;
    uint8_t chunk[256];

    for (;;) {
        int got = uart_read_bytes(port, chunk, sizeof(chunk), pdMS_TO_TICKS(200));
        for (int i = 0; i < got; i++) {
            char c = (char)chunk[i];
            if (c == '\n' || c == '\r') {
                if (len > 0) {
                    linebuf[role][len] = 0;
                    handle_line(role, linebuf[role]);
                    len = 0;
                }
            } else if (len < (int)sizeof(linebuf[role]) - 1) {
                linebuf[role][len++] = c;
            } else {
                len = 0;  
            }
        }
    }
}

static void lora_poll_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LORA_POLL_MS));
        uart_send_line(UART_LORA, "{\"cmd\":\"fetch\"}");
    }
}

esp_err_t uart_links_send_status(const char *line)
{
    uart_send_line(UART_STATUS, line);
    return ESP_OK;
}

esp_err_t uart_links_send_lora(const char *to, int ch, const char *text)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "cmd", "send");
    if (to && to[0]) cJSON_AddStringToObject(o, "to", to);
    cJSON_AddNumberToObject(o, "ch", ch);
    cJSON_AddStringToObject(o, "text", text ? text : "");
    char *s = cJSON_PrintUnformatted(o);
    if (s) { uart_send_line(UART_LORA, s); free(s); }
    cJSON_Delete(o);
    return ESP_OK;
}

void uart_links_get_nodes(bool *wroom2, bool *lora, bool *status)
{
    int64_t now = esp_timer_get_time() / 1000;
    #define ONLINE(r) (s_last_seen[r] >= 0 && (now - s_last_seen[r]) < NODE_OFFLINE_MS)
    if (wroom2) *wroom2 = ONLINE(ROLE_WROOM2);
    if (lora)   *lora   = ONLINE(ROLE_LORA);
    if (status) *status = ONLINE(ROLE_STATUS);
    #undef ONLINE
}

char *uart_links_get_env(int64_t *age_ms)
{
    char *copy = NULL;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_env_json) {
        copy = strdup(s_env_json);
        if (age_ms) *age_ms = esp_timer_get_time() / 1000 - s_env_time;
    }
    xSemaphoreGive(s_mtx);
    return copy;
}

esp_err_t uart_links_init(void)
{
    s_mtx = xSemaphoreCreateMutex();

    const struct { int port, tx, rx; } cfgs[3] = {
        { UART_WROOM2, UART_WROOM2_TX, UART_WROOM2_RX },
        { UART_LORA,   UART_LORA_TX,   UART_LORA_RX   },
        { UART_STATUS, UART_STATUS_TX, UART_STATUS_RX },
    };
    uart_config_t uc = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    for (int i = 0; i < 3; i++) {
        ESP_ERROR_CHECK(uart_driver_install(cfgs[i].port, 4096, 2048, 0, NULL, 0));
        ESP_ERROR_CHECK(uart_param_config(cfgs[i].port, &uc));
        ESP_ERROR_CHECK(uart_set_pin(cfgs[i].port, cfgs[i].tx, cfgs[i].rx,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    }

    static rx_arg_t args[3] = { {ROLE_WROOM2}, {ROLE_LORA}, {ROLE_STATUS} };
    xTaskCreate(rx_task, "rx_wroom2", 6144, &args[0], 6, NULL);
    xTaskCreate(rx_task, "rx_lora",   6144, &args[1], 6, NULL);
    xTaskCreate(rx_task, "rx_status", 6144, &args[2], 6, NULL);
    xTaskCreate(lora_poll_task, "lora_poll", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "3 UART links up (WROOM2 tx%d/rx%d, LoRa tx%d/rx%d, Status tx%d/rx%d)",
             UART_WROOM2_TX, UART_WROOM2_RX, UART_LORA_TX, UART_LORA_RX,
             UART_STATUS_TX, UART_STATUS_RX);
    return ESP_OK;
}
