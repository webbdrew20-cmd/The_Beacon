#include "http_media.h"
#include "wroom2_config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"

static const char *TAG = "http";
static httpd_handle_t s_server;
static QueueHandle_t s_mediaq;


static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = 0;
}

static const char *mime_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    dot++;
    if (!strcasecmp(dot, "html")) return "text/html";
    if (!strcasecmp(dot, "css"))  return "text/css";
    if (!strcasecmp(dot, "js"))   return "text/javascript";
    if (!strcasecmp(dot, "json")) return "application/json";
    if (!strcasecmp(dot, "wasm")) return "application/wasm";
    if (!strcasecmp(dot, "mp3"))  return "audio/mpeg";
    if (!strcasecmp(dot, "m4a"))  return "audio/mp4";
    if (!strcasecmp(dot, "wav"))  return "audio/wav";
    if (!strcasecmp(dot, "ogg"))  return "audio/ogg";
    if (!strcasecmp(dot, "png"))  return "image/png";
    if (!strcasecmp(dot, "jpg") || !strcasecmp(dot, "jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, "svg"))  return "image/svg+xml";
    if (!strcasecmp(dot, "ico"))  return "image/x-icon";
    if (!strcasecmp(dot, "gif"))  return "image/gif";
    return "application/octet-stream";
}


static void send_media(httpd_req_t *req, const char *fspath)
{
    FILE *f = fopen(fspath, "rb");
    if (!f) { httpd_resp_send_err(req, 404, "Not found"); return; }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);

    long start = 0, end = total - 1;
    bool ranged = false;
    char hv[96];
    if (httpd_req_get_hdr_value_str(req, "Range", hv, sizeof(hv)) == ESP_OK &&
        strncmp(hv, "bytes=", 6) == 0) {
        char *p = hv + 6;
        if (*p == '-') {                       
            long n = atol(p + 1);
            if (n > 0) { start = total - n; if (start < 0) start = 0; }
        } else {
            start = atol(p);
            char *d = strchr(p, '-');
            if (d && isdigit((unsigned char)d[1])) end = atol(d + 1);
        }
        if (end >= total) end = total - 1;
        if (start < 0 || start > total - 1 || end < start) {
            char h[160];
            int hl = snprintf(h, sizeof(h),
                "HTTP/1.1 416 Range Not Satisfiable\r\n"
                "Content-Range: bytes */%ld\r\nContent-Length: 0\r\n"
                "Connection: close\r\n\r\n", total);
            httpd_send(req, h, hl);
            fclose(f);
            return;
        }
        ranged = true;
    }

    long len = end - start + 1;
    char h[320];
    int hl;
    if (ranged)
        hl = snprintf(h, sizeof(h),
            "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\n"
            "Accept-Ranges: bytes\r\nContent-Range: bytes %ld-%ld/%ld\r\n"
            "Content-Length: %ld\r\nCache-Control: max-age=3600\r\n"
            "Connection: close\r\n\r\n",
            mime_for(fspath), start, end, total, len);
    else
        hl = snprintf(h, sizeof(h),
            "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
            "Accept-Ranges: bytes\r\nContent-Length: %ld\r\n"
            "Cache-Control: max-age=3600\r\nConnection: close\r\n\r\n",
            mime_for(fspath), total);
    if (httpd_send(req, h, hl) < 0) { fclose(f); return; }

    fseek(f, start, SEEK_SET);
    char *buf = malloc(16384);
    if (!buf) { fclose(f); return; }
    long remain = len;
    while (remain > 0) {
        size_t take = remain > 16384 ? 16384 : (size_t)remain;
        size_t n = fread(buf, 1, take, f);
        if (n == 0) break;
        if (httpd_send(req, buf, n) < 0) break;   
        remain -= n;
    }
    free(buf);
    fclose(f);
}


static bool media_fspath(const char *path, char *out, size_t cap)
{
    if (strncmp(path, "/music/files/", 13) == 0)
        snprintf(out, cap, MUSIC_DIR "/%s", path + 13);
    else if (strncmp(path, "/games/roms/", 12) == 0)
        snprintf(out, cap, ROMS_DIR "/%s", path + 12);
    else if (strncmp(path, "/games/ejs/", 11) == 0)
        snprintf(out, cap, EJS_DIR "/%s", path + 11);
    else
        return false;
    return true;
}

static void media_worker(void *arg)
{
    for (;;) {
        httpd_req_t *req = NULL;
        if (xQueueReceive(s_mediaq, &req, portMAX_DELAY) != pdTRUE) continue;

        char path[600];
        strlcpy(path, req->uri, sizeof(path));
        char *qm = strchr(path, '?');
        if (qm) *qm = 0;
        url_decode(path);

        char fspath[700];
        if (strstr(path, "..") || !media_fspath(path, fspath, sizeof(fspath)))
            httpd_resp_send_err(req, 400, "Bad media path");
        else
            send_media(req, fspath);

        httpd_req_async_handler_complete(req);
    }
}


static bool serve_small(httpd_req_t *req, const char *fspath)
{
    FILE *f = fopen(fspath, "rb");
    if (!f) return false;
    httpd_resp_set_type(req, mime_for(fspath));
    char *buf = malloc(8192);
    if (!buf) { fclose(f); httpd_resp_send_err(req, 500, "No memory"); return true; }
    size_t n;
    while ((n = fread(buf, 1, 8192, f)) > 0)
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
    free(buf);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return true;
}


static void send_dir_list(httpd_req_t *req, const char *dirpath,
                          const char *const *exts, int next)
{
    cJSON *arr = cJSON_CreateArray();
    DIR *d = opendir(dirpath);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *dot = strrchr(e->d_name, '.');
            if (!dot) continue;
            bool ok = false;
            for (int i = 0; i < next && !ok; i++)
                if (strcasecmp(dot + 1, exts[i]) == 0) ok = true;
            if (!ok) continue;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "file", e->d_name);
            char name[200];
            strlcpy(name, e->d_name, sizeof(name));
            char *nd = strrchr(name, '.');
            if (nd) *nd = 0;
            for (char *c = name; *c; c++) if (*c == '_') *c = ' ';
            cJSON_AddStringToObject(o, "name", name);
            cJSON_AddItemToArray(arr, o);
        }
        closedir(d);
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json ? json : "[]", HTTPD_RESP_USE_STRLEN);
    free(json);
}


static esp_err_t root_handler(httpd_req_t *req)
{
    char path[600];
    strlcpy(path, req->uri, sizeof(path));
    char *qm = strchr(path, '?');
    if (qm) *qm = 0;
    url_decode(path);
    if (strstr(path, "..")) { httpd_resp_send_err(req, 400, "Bad path"); return ESP_OK; }

   
    if (strncmp(path, "/music/files/", 13) == 0 ||
        strncmp(path, "/games/roms/", 12) == 0 ||
        strncmp(path, "/games/ejs/", 11) == 0) {
        httpd_req_t *aq = NULL;
        if (httpd_req_async_handler_begin(req, &aq) != ESP_OK) {
            httpd_resp_send_err(req, 500, "Async begin failed");
            return ESP_OK;
        }
        if (xQueueSend(s_mediaq, &aq, 0) != pdTRUE) {
            httpd_resp_set_status(aq, "503 Busy");
            httpd_resp_sendstr(aq, "Busy — try again in a moment");
            httpd_req_async_handler_complete(aq);
        }
        return ESP_OK;
    }

    static const char *music_exts[] = { "mp3", "m4a", "wav", "ogg" };
    static const char *rom_exts[]   = { "gb", "gbc", "nes" };

    if (strcmp(path, "/music/list") == 0) {
        send_dir_list(req, MUSIC_DIR, music_exts, 4);
        return ESP_OK;
    }
    if (strcmp(path, "/games/list") == 0) {
        send_dir_list(req, ROMS_DIR, rom_exts, 3);
        return ESP_OK;
    }

    if (strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/music/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    if (strcmp(path, "/music") == 0 || strcmp(path, "/music/") == 0) {
        if (serve_small(req, WWW_ROOT "/music.html")) return ESP_OK;
    }
    if (strcmp(path, "/games") == 0 || strcmp(path, "/games/") == 0) {
        if (serve_small(req, WWW_ROOT "/games.html")) return ESP_OK;
    }
    if (strcmp(path, "/games/play.html") == 0) {
        if (serve_small(req, WWW_ROOT "/play.html")) return ESP_OK;
    }

   
    char fspath[700];
    snprintf(fspath, sizeof(fspath), WWW_ROOT "%s", path);
    if (serve_small(req, fspath)) return ESP_OK;

    httpd_resp_send_err(req, 404,
        "Not found. If pages are missing, check the SD card layout "
        "(see the README).");
    return ESP_OK;
}

esp_err_t http_media_start(void)
{
    s_mediaq = xQueueCreate(MEDIA_WORKQ_DEPTH, sizeof(httpd_req_t *));
    for (int i = 0; i < MEDIA_WORKERS; i++)
        xTaskCreate(media_worker, "media_worker", 6144, NULL, 5, NULL);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_open_sockets = 10;
    cfg.stack_size = 12288;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) return err;

    httpd_uri_t u_all = { .uri = "/*", .method = HTTP_GET, .handler = root_handler };
    httpd_register_uri_handler(s_server, &u_all);

    ESP_LOGI(TAG, "media server up on :80");
    return ESP_OK;
}
