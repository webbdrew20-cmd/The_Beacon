#include "msg_log.h"
#include "beacon_config.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sd_mount.h"

static const char *TAG = "msglog";

static beacon_msg_t *s_ring;      
static int s_count = 0, s_next_slot = 0;
static uint32_t s_next_id = 1;
static int s_chunk_no = 1;
static long s_chunk_size = 0;
static SemaphoreHandle_t s_mtx;

static void chunk_path(char *out, size_t cap, int no)
{
    snprintf(out, cap, MSG_DIR "/log-%06d.jsonl", no);
}


static void jesc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 2 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c < 0x20)         { out[o++] = ' '; }
        else                       { out[o++] = c; }
    }
    out[o] = 0;
}

esp_err_t msg_log_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_ring = heap_caps_calloc(MSG_RING_CAP, sizeof(beacon_msg_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) return ESP_ERR_NO_MEM;

    sd_lock();
    mkdir(MSG_DIR, 0775);

    
    DIR *d = opendir(MSG_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            int n = 0;
            if (sscanf(e->d_name, "log-%06d.jsonl", &n) == 1 && n > s_chunk_no)
                s_chunk_no = n;
        }
        closedir(d);
    }
    char p[96];
    chunk_path(p, sizeof(p), s_chunk_no);
    struct stat st;
    if (stat(p, &st) == 0) {
        s_chunk_size = st.st_size;
        // Read the tail to recover the highest id written so far.
        FILE *f = fopen(p, "rb");
        if (f) {
            long off = st.st_size > 4096 ? st.st_size - 4096 : 0;
            fseek(f, off, SEEK_SET);
            static char tail[4100];
            size_t got = fread(tail, 1, sizeof(tail) - 1, f);
            tail[got] = 0;
            fclose(f);
            char *q = tail;
            while ((q = strstr(q, "\"id\":")) != NULL) {
                uint32_t v = (uint32_t)strtoul(q + 5, NULL, 10);
                if (v >= s_next_id) s_next_id = v + 1;
                q += 5;
            }
        }
    }
    sd_unlock();
    ESP_LOGI(TAG, "chunk %d (%ld bytes), next id %u",
             s_chunk_no, s_chunk_size, (unsigned)s_next_id);
    return ESP_OK;
}


static void enforce_total_cap(void)
{
    for (;;) {
        long long total = 0;
        int oldest = 0x7fffffff, count = 0;
        DIR *d = opendir(MSG_DIR);
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            int n = 0;
            if (sscanf(e->d_name, "log-%06d.jsonl", &n) != 1) continue;
            char p[96]; chunk_path(p, sizeof(p), n);
            struct stat st;
            if (stat(p, &st) == 0) total += st.st_size;
            if (n < oldest) oldest = n;
            count++;
        }
        closedir(d);
        if (total <= MSG_TOTAL_MAX_BYTES || count <= 1) return;
        char p[96]; chunk_path(p, sizeof(p), oldest);
        ESP_LOGW(TAG, "1GB cap hit — deleting %s", p);
        unlink(p);
    }
}

uint32_t msg_log_append(char dir, const char *from, int ch, const char *text)
{
    if (!text) text = "";
    if (!from) from = "";
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    uint32_t id = s_next_id++;
    int64_t t_ms = esp_timer_get_time() / 1000;

    char ef[96], et[520], line[720], p[96];
    jesc(from, ef, sizeof(ef));
    jesc(text, et, sizeof(et));
    int len = snprintf(line, sizeof(line),
        "{\"id\":%u,\"dir\":\"%s\",\"from\":\"%s\",\"ch\":%d,\"text\":\"%s\",\"t\":%lld}\n",
        (unsigned)id, dir == 'o' ? "out" : "in", ef, ch, et, (long long)t_ms);

    sd_lock();
    chunk_path(p, sizeof(p), s_chunk_no);
    FILE *f = fopen(p, "ab");
    if (f) { fwrite(line, 1, len, f); fclose(f); s_chunk_size += len; }
    if (s_chunk_size >= MSG_CHUNK_MAX_BYTES) {
        s_chunk_no++;
        s_chunk_size = 0;
        enforce_total_cap();
    }
    sd_unlock();

    
    beacon_msg_t *m = &s_ring[s_next_slot];
    m->id = id; m->dir = dir; m->ch = ch; m->t_ms = t_ms;
    strlcpy(m->from, from, sizeof(m->from));
    strlcpy(m->text, text, sizeof(m->text));
    s_next_slot = (s_next_slot + 1) % MSG_RING_CAP;
    if (s_count < MSG_RING_CAP) s_count++;

    xSemaphoreGive(s_mtx);
    return id;
}

int msg_log_since(uint32_t since_id, beacon_msg_t *out, int max)
{
    int n = 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int start = (s_next_slot - s_count + MSG_RING_CAP) % MSG_RING_CAP;
    for (int i = 0; i < s_count && n < max; i++) {
        beacon_msg_t *m = &s_ring[(start + i) % MSG_RING_CAP];
        if (m->id > since_id) out[n++] = *m;
    }
    xSemaphoreGive(s_mtx);
    return n;
}
