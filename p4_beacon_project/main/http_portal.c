#include "http_portal.h"
#include "beacon_config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"
#include "sd_mount.h"
#include "zim_reader.h"
#include "msg_log.h"
#include "uart_links.h"

static const char *TAG = "http";
static httpd_handle_t s_server;

static void url_decode(char *s, bool plus_is_space);   // defined below


static QueueHandle_t s_zimq;


static void zim_dispatch(httpd_req_t *req, char *path)
{
    char *name = path + 9;
    char *slash = strchr(name, '/');
    char rest_default[1] = "";
    char *rest = rest_default;
    if (slash) { *slash = 0; rest = slash + 1; }
    zim_t *z = zim_by_name(name);
    if (!z) { httpd_resp_send_err(req, 404, "No such archive"); return; }
    zim_serve_path(req, z, rest);
}

static void zim_worker(void *arg)
{
    for (;;) {
        httpd_req_t *req = NULL;
        if (xQueueReceive(s_zimq, &req, portMAX_DELAY) != pdTRUE) continue;

        char path[600];
        strlcpy(path, req->uri, sizeof(path));
        char *qm = strchr(path, '?');
        if (qm) *qm = 0;
        url_decode(path, false);

        if (strstr(path, "..")) httpd_resp_send_err(req, 400, "Bad path");
        else zim_dispatch(req, path);

        httpd_req_async_handler_complete(req);
    }
}


static void url_decode(char *s, bool plus_is_space)
{
    char *o = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else if (*s == '+' && plus_is_space) {
            *o++ = ' '; s++;
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
    if (!strcasecmp(dot, "html") || !strcasecmp(dot, "htm")) return "text/html";
    if (!strcasecmp(dot, "css"))  return "text/css";
    if (!strcasecmp(dot, "js"))   return "application/javascript";
    if (!strcasecmp(dot, "json")) return "application/json";
    if (!strcasecmp(dot, "png"))  return "image/png";
    if (!strcasecmp(dot, "jpg") || !strcasecmp(dot, "jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, "gif"))  return "image/gif";
    if (!strcasecmp(dot, "svg"))  return "image/svg+xml";
    if (!strcasecmp(dot, "ico"))  return "image/x-icon";
    if (!strcasecmp(dot, "webp")) return "image/webp";
    if (!strcasecmp(dot, "txt"))  return "text/plain";
    if (!strcasecmp(dot, "pdf"))  return "application/pdf";
    if (!strcasecmp(dot, "mp3"))  return "audio/mpeg";
    if (!strcasecmp(dot, "woff2"))return "font/woff2";
    return "application/octet-stream";
}

static void html_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 6 < cap; in++) {
        if (*in == '<')      o += snprintf(out + o, cap - o, "&lt;");
        else if (*in == '>') o += snprintf(out + o, cap - o, "&gt;");
        else if (*in == '&') o += snprintf(out + o, cap - o, "&amp;");
        else if (*in == '"') o += snprintf(out + o, cap - o, "&quot;");
        else out[o++] = *in;
    }
    out[o] = 0;
}


static bool serve_file(httpd_req_t *req, const char *fspath)
{
    sd_lock();
    FILE *f = fopen(fspath, "rb");
    sd_unlock();
    if (!f) return false;

    httpd_resp_set_type(req, mime_for(fspath));
    char *buf = heap_caps_malloc(32768, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); httpd_resp_send_err(req, 500, "No memory"); return true; }

    for (;;) {
        sd_lock();
        size_t n = fread(buf, 1, 32768, f);
        sd_unlock();
        if (n == 0) break;
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
    }
    free(buf);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return true;
}


static esp_err_t captive_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}


static esp_err_t search_handler(httpd_req_t *req)
{
    char qbuf[160] = "", q[128] = "";
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK)
        httpd_query_key_value(qbuf, "q", q, sizeof(q));
    url_decode(q, true);

    size_t cap = 32768, len = 0;
    char *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) { httpd_resp_send_err(req, 500, "No memory"); return ESP_OK; }
    #define APP(...) do { if (len < cap) len += snprintf(out + len, cap - len, __VA_ARGS__); } while (0)

    char qesc[280];
    html_escape(q, qesc, sizeof(qesc));
    APP("<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Search — Beacon</title><style>"
        "body{background:#16273B;color:#EDE8DA;font-family:Georgia,serif;"
        "max-width:640px;margin:0 auto;padding:24px 16px}"
        "a{color:#C99A3B;text-decoration:none}a:hover{text-decoration:underline}"
        ".hit{padding:10px 0;border-bottom:1px solid rgba(237,232,218,0.12)}"
        ".src{font-size:0.75rem;opacity:0.55;letter-spacing:0.06em;text-transform:uppercase}"
        "h1{font-size:1.3rem;font-weight:normal}</style></head><body>"
        "<p><a href='/'>&larr; Beacon</a></p><h1>Results for \"%s\"</h1>", qesc);

    int total = 0;
    if (q[0]) {
        zim_hit_t *hits = heap_caps_malloc(sizeof(zim_hit_t) * ZIM_SEARCH_PER_FILE,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        for (int pass = 0; pass < 2 && total == 0 && hits; pass++) {
            char term[128];
            strlcpy(term, q, sizeof(term));
            if (pass == 1) {           // retry Capitalized (wiki convention)
                if (term[0] >= 'a' && term[0] <= 'z') term[0] -= 32;
                else break;
            }
            for (int i = 0; i < zim_count(); i++) {
                int n = zim_search(zim_at(i), term, hits, ZIM_SEARCH_PER_FILE);
                for (int j = 0; j < n; j++) {
                    char tesc[280];
                    html_escape(hits[j].title, tesc, sizeof(tesc));
                    APP("<div class='hit'><a href='/infoweb/%s/%s'>%s</a>"
                        "<div class='src'>%s</div></div>",
                        zim_name(i), hits[j].path, tesc, zim_name(i));
                    total++;
                }
            }
        }
        free(hits);
    }
    if (total == 0)
        APP("<p style='opacity:0.6'>No matching article titles. Try different "
            "words, or check spelling — search matches how titles begin.</p>");
    APP("</body></html>");
    #undef APP

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, out, len);
    free(out);
    return ESP_OK;
}


static esp_err_t api_messages_handler(httpd_req_t *req)
{
    char qbuf[64] = "", sv[24] = "0";
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK &&
        httpd_query_key_value(qbuf, "since", sv, sizeof(sv)) == ESP_OK)
        since = (uint32_t)strtoul(sv, NULL, 10);

    beacon_msg_t *msgs = heap_caps_malloc(sizeof(beacon_msg_t) * 100,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!msgs) { httpd_resp_send_err(req, 500, "No memory"); return ESP_OK; }
    int n = msg_log_since(since, msgs, 100);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", msgs[i].id);
        cJSON_AddStringToObject(o, "dir", msgs[i].dir == 'o' ? "out" : "in");
        cJSON_AddStringToObject(o, "from", msgs[i].from);
        cJSON_AddNumberToObject(o, "ch", msgs[i].ch);
        cJSON_AddStringToObject(o, "text", msgs[i].text);
        cJSON_AddNumberToObject(o, "t", (double)msgs[i].t_ms);
        cJSON_AddItemToArray(arr, o);
    }
    free(msgs);
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json ? json : "[]", HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t api_send_handler(httpd_req_t *req)
{
    char body[1024];
    int got = httpd_req_recv(req, body, sizeof(body) - 1);
    if (got <= 0) { httpd_resp_send_err(req, 400, "Empty body"); return ESP_OK; }
    body[got] = 0;

    cJSON *root = cJSON_Parse(body);
    const cJSON *text = root ? cJSON_GetObjectItem(root, "text") : NULL;
    const cJSON *to   = root ? cJSON_GetObjectItem(root, "to") : NULL;
    const cJSON *ch   = root ? cJSON_GetObjectItem(root, "ch") : NULL;

    if (!cJSON_IsString(text) || text->valuestring[0] == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, 400, "Missing text");
        return ESP_OK;
    }
    const char *to_s = cJSON_IsString(to) ? to->valuestring : "";
    int ch_n = cJSON_IsNumber(ch) ? ch->valueint : 0;

    uart_links_send_lora(to_s, ch_n, text->valuestring);
    uint32_t id = msg_log_append('o', to_s, ch_n, text->valuestring);
    cJSON_Delete(root);

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"id\":%u}", (unsigned)id);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


static esp_err_t api_altitude_handler(httpd_req_t *req)
{
    char body[128];
    int got = httpd_req_recv(req, body, sizeof(body) - 1);
    if (got <= 0) { httpd_resp_send_err(req, 400, "Empty body"); return ESP_OK; }
    body[got] = 0;

    cJSON *root = cJSON_Parse(body);
    const cJSON *m = root ? cJSON_GetObjectItem(root, "m") : NULL;
    if (!cJSON_IsNumber(m) || m->valuedouble < 0 || m->valuedouble > 4500) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, 400, "Altitude must be 0-4500 meters");
        return ESP_OK;
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"set_alt\",\"m\":%.0f}", m->valuedouble);
    uart_links_send_status(cmd);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_status_handler(httpd_req_t *req)
{
    bool w, l, s;
    uart_links_get_nodes(&w, &l, &s);
    int64_t age = -1;
    char *env = uart_links_get_env(&age);

    cJSON *o = cJSON_CreateObject();
    cJSON *nodes = cJSON_AddObjectToObject(o, "nodes");
    cJSON_AddBoolToObject(nodes, "wroom2", w);
    cJSON_AddBoolToObject(nodes, "lora", l);
    cJSON_AddBoolToObject(nodes, "status", s);
    if (env) {
        cJSON *ej = cJSON_Parse(env);
        cJSON_AddItemToObject(o, "env", ej ? ej : cJSON_CreateNull());
        cJSON_AddNumberToObject(o, "env_age_ms", (double)age);
        free(env);
    } else {
        cJSON_AddItemToObject(o, "env", cJSON_CreateNull());
    }
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json ? json : "{}", HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}


static esp_err_t root_handler(httpd_req_t *req)
{
    char path[600];
    strlcpy(path, req->uri, sizeof(path));
    char *qm = strchr(path, '?');
    if (qm) *qm = 0;
    url_decode(path, false);

    if (strstr(path, "..")) { httpd_resp_send_err(req, 400, "Bad path"); return ESP_OK; }

    
    if (strncmp(path, "/infoweb/", 9) == 0) {
        httpd_req_t *aq = NULL;
        if (httpd_req_async_handler_begin(req, &aq) != ESP_OK) {
            httpd_resp_send_err(req, 500, "Async begin failed");
            return ESP_OK;
        }
        if (xQueueSend(s_zimq, &aq, 0) != pdTRUE) {
            httpd_resp_set_status(aq, "503 Busy");
            httpd_resp_set_type(aq, "text/html");
            httpd_resp_sendstr(aq,
                "<html><body style='background:#16273B;color:#EDE8DA;"
                "font-family:sans-serif;padding:24px'>Beacon is busy serving "
                "other readers — try again in a moment.</body></html>");
            httpd_req_async_handler_complete(aq);
        }
        return ESP_OK;
    }

    char fspath[700];
    
    if (strcmp(path, "/") == 0)
        snprintf(fspath, sizeof(fspath), WWW_ROOT "/index.html");
    else
        snprintf(fspath, sizeof(fspath), WWW_ROOT "%s", path);
    if (serve_file(req, fspath)) return ESP_OK;

   
    if (strcmp(path, "/") == 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req,
            "<html><body style='font-family:sans-serif;background:#16273B;"
            "color:#EDE8DA;padding:24px'>"
            "<h2>Beacon is running &mdash; but the SD card is missing its pages.</h2>"
            "<p>Expected: <b>/www/index.html</b> at the top level of the card.</p>"
            "<pre style='color:#C99A3B'>/www/               index.html, sites.html, books.html, messages.html\n"
            "/content/infoweb/   the .zim files\n"
            "/content/library/   book folders</pre>"
            "<p>Power off, fix the card layout on a PC, reinsert, reboot.</p>"
            "</body></html>",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    
    if (strncmp(path, "/content/", 9) == 0) {
        size_t plen = strlen(path);
        if (plen > 4 && strcasecmp(path + plen - 4, ".zim") == 0) {
            httpd_resp_send_err(req, 403, "Archives are browsed, not downloaded");
            return ESP_OK;
        }
        snprintf(fspath, sizeof(fspath), SD_MOUNT "%s", path);
        if (serve_file(req, fspath)) return ESP_OK;
        // Folder link convenience: try index.html inside
        snprintf(fspath, sizeof(fspath), SD_MOUNT "%s/index.html", path);
        if (serve_file(req, fspath)) return ESP_OK;
    }

   
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}


esp_err_t http_portal_start(void)
{
    s_zimq = xQueueCreate(ZIM_WORKQ_DEPTH, sizeof(httpd_req_t *));
    for (int i = 0; i < ZIM_WORKERS; i++)
        xTaskCreate(zim_worker, "zim_worker", 10240, NULL, 5, NULL);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 16;
    cfg.max_open_sockets = 10;
    cfg.stack_size = 20480;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) return err;

    
    const char *captive[] = {
        "/generate_204", "/gen_204",                       // Android
        "/hotspot-detect.html", "/library/test/success.html", // Apple
        "/connecttest.txt", "/ncsi.txt",                   // Windows
        "/canonical.html", "/success.txt",                 // Firefox etc
    };
    for (size_t i = 0; i < sizeof(captive) / sizeof(captive[0]); i++) {
        httpd_uri_t u = { .uri = captive[i], .method = HTTP_GET, .handler = captive_handler };
        httpd_register_uri_handler(s_server, &u);
    }

    httpd_uri_t u_search = { .uri = "/search",       .method = HTTP_GET,  .handler = search_handler };
    httpd_uri_t u_msgs   = { .uri = "/api/messages", .method = HTTP_GET,  .handler = api_messages_handler };
    httpd_uri_t u_send   = { .uri = "/api/send",     .method = HTTP_POST, .handler = api_send_handler };
    httpd_uri_t u_stat   = { .uri = "/api/status",   .method = HTTP_GET,  .handler = api_status_handler };
    httpd_uri_t u_alt    = { .uri = "/api/altitude", .method = HTTP_POST, .handler = api_altitude_handler };
    httpd_uri_t u_all    = { .uri = "/*",            .method = HTTP_GET,  .handler = root_handler };
    httpd_register_uri_handler(s_server, &u_search);
    httpd_register_uri_handler(s_server, &u_msgs);
    httpd_register_uri_handler(s_server, &u_send);
    httpd_register_uri_handler(s_server, &u_stat);
    httpd_register_uri_handler(s_server, &u_alt);
    httpd_register_uri_handler(s_server, &u_all);

    ESP_LOGI(TAG, "portal up on :80");
    return ESP_OK;
}
