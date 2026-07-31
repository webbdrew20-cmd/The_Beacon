#include "zim_reader.h"
#include "beacon_config.h"
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ff.h"
#include "zstd.h"
#include "sd_mount.h"

static const char *TAG = "zim";

typedef struct zim_file {
    FIL      f;
    bool     open;
    uint8_t  id;
    char     name[96];          
    uint16_t minor;            
    uint32_t entryCount, clusterCount, mainPage;
    uint64_t urlPtrPos, titlePtrPos, clusterPtrPos, mimeListPos, checksumPos;
    char    *mimeblob;
    const char *mimes[80];
    int      nmime;
} zim_t;

static zim_t s_zims[ZIM_MAX_FILES];
static int   s_nzims = 0;


static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }


static bool zread(zim_t *z, uint64_t off, void *buf, size_t len, size_t *out_br)
{
    UINT br = 0;
    sd_lock();
    FRESULT fr = f_lseek(&z->f, (FSIZE_t)off);
    if (fr == FR_OK) fr = f_read(&z->f, buf, len, &br);
    sd_unlock();
    if (out_br) *out_br = br;
    return fr == FR_OK;
}


typedef struct {
    bool     redirect;
    uint16_t mime;
    char     ns;
    uint32_t redirect_index;
    uint32_t cluster, blob;
    char     url[512];
    char     title[256];
} dirent_z_t;

static bool read_dirent_at(zim_t *z, uint64_t off, dirent_z_t *d)
{
    uint8_t buf[1024];
    size_t br;
    if (!zread(z, off, buf, sizeof(buf), &br) || br < 16) return false;

    d->mime = rd16(buf);
    if (d->mime == 0xFFFE || d->mime == 0xFFFD) return false;  
    d->redirect = (d->mime == 0xFFFF);
    d->ns = (char)buf[3];

    size_t fixed;
    if (d->redirect) {
        d->redirect_index = rd32(buf + 8);
        fixed = 12;
    } else {
        d->cluster = rd32(buf + 8);
        d->blob    = rd32(buf + 12);
        fixed = 16;
    }

    size_t i = fixed, o = 0;
    while (i < br && buf[i] && o < sizeof(d->url) - 1) d->url[o++] = buf[i++];
    d->url[o] = 0;
    if (i >= br) return false;
    i++;
    o = 0;
    while (i < br && buf[i] && o < sizeof(d->title) - 1) d->title[o++] = buf[i++];
    d->title[o] = 0;
    if (d->title[0] == 0) strlcpy(d->title, d->url, sizeof(d->title));
    return true;
}

static bool read_dirent_by_index(zim_t *z, uint32_t idx, dirent_z_t *d)
{
    if (idx >= z->entryCount) return false;
    uint8_t p[8];
    size_t br;
    if (!zread(z, z->urlPtrPos + 8ULL * idx, p, 8, &br) || br != 8) return false;
    return read_dirent_at(z, rd64(p), d);
}


typedef struct {
    uint32_t idx;
    uint8_t  zid;
    uint8_t  klen;
    uint8_t  used;
    uint8_t  complete;         
    char     ns;
    char     key[ZIM_DKEY_LEN];
} dkey_t;

static dkey_t *s_dkeys;
static SemaphoreHandle_t s_dk_mtx;

static uint32_t dk_hash(uint8_t zid, uint32_t idx)
{
    uint32_t h = (idx * 2654435761u) ^ ((uint32_t)zid * 40503u);
    return h & (ZIM_DKEY_ENTRIES - 1);
}

static void dk_insert(zim_t *z, uint32_t idx, const dirent_z_t *d)
{
    if (!s_dkeys) return;
    size_t ul = strlen(d->url);
    xSemaphoreTake(s_dk_mtx, portMAX_DELAY);
    uint32_t h = dk_hash(z->id, idx);
    dkey_t *e = &s_dkeys[h];              
    e->idx = idx; e->zid = z->id; e->used = 1;
    e->ns = d->ns;
    e->complete = (ul < ZIM_DKEY_LEN);
    e->klen = e->complete ? ul : ZIM_DKEY_LEN;
    memcpy(e->key, d->url, e->klen);
    if (e->complete) e->key[e->klen] = 0;
    xSemaphoreGive(s_dk_mtx);
}

// Returns true if the cache resolved the comparison decisively.
static bool dk_try(zim_t *z, uint32_t idx, char ns, const char *url, int *cmp)
{
    if (!s_dkeys) return false;
    bool decisive = false;
    xSemaphoreTake(s_dk_mtx, portMAX_DELAY);
    dkey_t *e = &s_dkeys[dk_hash(z->id, idx)];
    if (e->used && e->zid == z->id && e->idx == idx) {
        if (e->ns != ns) {
            *cmp = ((unsigned char)e->ns < (unsigned char)ns) ? -1 : 1;
            decisive = true;
        } else if (e->complete) {
            char k[ZIM_DKEY_LEN + 1];
            memcpy(k, e->key, e->klen); k[e->klen] = 0;
            *cmp = strcmp(k, url);
            decisive = true;
        } else {
            int c = strncmp(e->key, url, ZIM_DKEY_LEN);
            if (c != 0) { *cmp = c; decisive = true; }
            // equal prefix on a truncated key -> not decisive, hit disk
        }
    }
    xSemaphoreGive(s_dk_mtx);
    return decisive;
}


static int cmp_entry(const dirent_z_t *d, char ns, const char *url)
{
    if (d->ns != ns) return (unsigned char)d->ns < (unsigned char)ns ? -1 : 1;
    return strcmp(d->url, url);
}

static bool zim_find(zim_t *z, char ns, const char *url, dirent_z_t *out)
{
    uint32_t lo = 0, hi = z->entryCount;
    dirent_z_t d;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int cmp;
        if (!dk_try(z, mid, ns, url, &cmp)) {
            if (!read_dirent_by_index(z, mid, &d)) return false;
            dk_insert(z, mid, &d);
            cmp = cmp_entry(&d, ns, url);
            if (cmp == 0) { *out = d; return true; }
        } else if (cmp == 0) {
            // Shouldn't happen (equal is never decisive from cache), but
            // resolve safely via disk if it ever does.
            if (!read_dirent_by_index(z, mid, &d)) return false;
            cmp = cmp_entry(&d, ns, url);
            if (cmp == 0) { *out = d; return true; }
        }
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

static bool resolve_redirects(zim_t *z, dirent_z_t *d)
{
    for (int depth = 0; depth < 4 && d->redirect; depth++) {
        if (!read_dirent_by_index(z, d->redirect_index, d)) return false;
    }
    return !d->redirect;
}


static const char *mime_of(zim_t *z, uint16_t m)
{
    if (m < z->nmime) return z->mimes[m];
    return "application/octet-stream";
}

static bool cluster_bounds(zim_t *z, uint32_t cluster,
                           uint64_t *coff, uint64_t *cend)
{
    if (cluster >= z->clusterCount) return false;
    uint8_t p[16];
    size_t br;
    size_t want = (cluster + 1 < z->clusterCount) ? 16 : 8;
    if (!zread(z, z->clusterPtrPos + 8ULL * cluster, p, want, &br) || br != want)
        return false;
    *coff = rd64(p);
    *cend = (cluster + 1 < z->clusterCount) ? rd64(p + 8) : z->checksumPos;
    return *cend > *coff;
}


typedef struct {
    zim_t   *z;
    uint32_t cluster;
    uint8_t *buf;
    size_t   size;
    int      osz;
    int64_t  stamp;
} ccache_t;

static ccache_t s_cc[ZIM_CCACHE_SLOTS];
static size_t   s_cc_total = 0;
static SemaphoreHandle_t s_cc_mtx;


static esp_err_t slice_blob(const uint8_t *buf, size_t size, int osz,
                            uint32_t blob, uint8_t **out, size_t *out_len)
{
    if (size < (size_t)osz) return ESP_FAIL;
    uint64_t off0 = (osz == 8) ? rd64(buf) : rd32(buf);
    uint64_t nblobs = off0 / osz - 1;
    if (blob >= nblobs) return ESP_FAIL;
    if (size < (size_t)(blob + 2) * osz) return ESP_FAIL;
    uint64_t a = (osz == 8) ? rd64(buf + (size_t)blob * osz)
                            : rd32(buf + (size_t)blob * osz);
    uint64_t b = (osz == 8) ? rd64(buf + (size_t)(blob + 1) * osz)
                            : rd32(buf + (size_t)(blob + 1) * osz);
    if (b < a || b > size) return ESP_FAIL;
    size_t len = b - a;
    uint8_t *copy = heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, buf + a, len);
    *out = copy;
    *out_len = len;
    return ESP_OK;
}


static esp_err_t decompress_cluster(zim_t *z, uint64_t cstart, uint64_t cend,
                                    size_t cap, uint8_t **out, size_t *out_len)
{
    esp_err_t ret = ESP_FAIL;
    size_t incap = 65536;
    uint8_t *inbuf = heap_caps_malloc(incap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t outcap = 262144, outlen = 0;
    uint8_t *outbuf = heap_caps_malloc(outcap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ZSTD_DStream *ds = ZSTD_createDStream();
    if (!inbuf || !outbuf || !ds) goto done;
    ZSTD_initDStream(ds);

    uint64_t pos = cstart, remain = cend - cstart;
    ZSTD_inBuffer zin = { inbuf, 0, 0 };
    for (;;) {
        if (zin.pos == zin.size) {
            if (remain == 0) break;
            size_t take = remain > incap ? incap : (size_t)remain;
            size_t br;
            if (!zread(z, pos, inbuf, take, &br) || br != take) goto done;
            pos += take; remain -= take;
            zin.src = inbuf; zin.size = take; zin.pos = 0;
        }
        if (outlen == outcap) {
            if (outcap >= cap) { ret = ESP_ERR_NO_MEM; goto done; }  
            size_t ncap = outcap * 2;
            if (ncap > cap) ncap = cap;
            uint8_t *nb = heap_caps_realloc(outbuf, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nb) goto done;
            outbuf = nb; outcap = ncap;
        }
        ZSTD_outBuffer zout = { outbuf, outcap, outlen };
        size_t r = ZSTD_decompressStream(ds, &zout, &zin);
        outlen = zout.pos;
        if (ZSTD_isError(r)) {
            ESP_LOGE(TAG, "zstd: %s", ZSTD_getErrorName(r));
            goto done;
        }
        if (r == 0) break;   
    }
    *out = outbuf;
    *out_len = outlen;
    ZSTD_freeDStream(ds);
    free(inbuf);
    return ESP_OK;

done:
    if (ds) ZSTD_freeDStream(ds);
    free(inbuf);
    free(outbuf);
    return ret;
}


static esp_err_t zstd_blob_copy(zim_t *z, uint32_t cluster,
                                uint64_t cstart, uint64_t cend, int osz,
                                uint32_t blob, uint8_t **out, size_t *out_len,
                                bool *cache_hit)
{
    *cache_hit = false;

    
    xSemaphoreTake(s_cc_mtx, portMAX_DELAY);
    for (int i = 0; i < ZIM_CCACHE_SLOTS; i++) {
        if (s_cc[i].buf && s_cc[i].z == z && s_cc[i].cluster == cluster) {
            s_cc[i].stamp = esp_timer_get_time();
            esp_err_t r = slice_blob(s_cc[i].buf, s_cc[i].size, s_cc[i].osz,
                                     blob, out, out_len);
            xSemaphoreGive(s_cc_mtx);
            *cache_hit = true;
            return r;
        }
    }
    xSemaphoreGive(s_cc_mtx);

    
    uint8_t *cbuf = NULL;
    size_t csize = 0;
    esp_err_t r = decompress_cluster(z, cstart, cend, ZIM_CCACHE_MAX_CLUSTER,
                                     &cbuf, &csize);
    if (r == ESP_ERR_NO_MEM) return r;     
    if (r != ESP_OK) return ESP_FAIL;

    r = slice_blob(cbuf, csize, osz, blob, out, out_len);

   
    xSemaphoreTake(s_cc_mtx, portMAX_DELAY);
    while (s_cc_total + csize > ZIM_CCACHE_BUDGET) {
        int lru = -1;
        int64_t oldest = INT64_MAX;
        for (int i = 0; i < ZIM_CCACHE_SLOTS; i++)
            if (s_cc[i].buf && s_cc[i].stamp < oldest) { oldest = s_cc[i].stamp; lru = i; }
        if (lru < 0) break;
        s_cc_total -= s_cc[lru].size;
        free(s_cc[lru].buf);
        s_cc[lru].buf = NULL;
    }
    int slot = -1;
    for (int i = 0; i < ZIM_CCACHE_SLOTS; i++)
        if (!s_cc[i].buf) { slot = i; break; }
    if (slot < 0) {
        int lru = 0;
        for (int i = 1; i < ZIM_CCACHE_SLOTS; i++)
            if (s_cc[i].stamp < s_cc[lru].stamp) lru = i;
        s_cc_total -= s_cc[lru].size;
        free(s_cc[lru].buf);
        s_cc[lru].buf = NULL;
        slot = lru;
    }
    if (s_cc_total + csize <= ZIM_CCACHE_BUDGET) {
        s_cc[slot] = (ccache_t){ .z = z, .cluster = cluster, .buf = cbuf,
                                 .size = csize, .osz = osz,
                                 .stamp = esp_timer_get_time() };
        s_cc_total += csize;
        cbuf = NULL;                        // ownership moved to cache
    }
    xSemaphoreGive(s_cc_mtx);
    free(cbuf);                             // only if cache declined it
    return r;
}


static esp_err_t send_zstd_stream(httpd_req_t *req, zim_t *z,
                                  uint64_t cstart, uint64_t cend, int osz,
                                  uint32_t blob)
{
    esp_err_t ret = ESP_FAIL;
    size_t incap = 65536;
    uint8_t *inbuf = heap_caps_malloc(incap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t outcap = 262144, outlen = 0;
    uint8_t *outbuf = heap_caps_malloc(outcap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ZSTD_DStream *ds = ZSTD_createDStream();
    if (!inbuf || !outbuf || !ds) goto done;
    ZSTD_initDStream(ds);

    uint64_t pos = cstart, remain = cend - cstart;
    bool have_count = false, have_range = false;
    uint64_t nblobs = 0, bstart = 0, bend = 0;

    ZSTD_inBuffer zin = { inbuf, 0, 0 };
    for (;;) {
        if (zin.pos == zin.size) {
            if (remain == 0) break;
            size_t take = remain > incap ? incap : (size_t)remain;
            size_t br;
            if (!zread(z, pos, inbuf, take, &br) || br != take) goto done;
            pos += take; remain -= take;
            zin.src = inbuf; zin.size = take; zin.pos = 0;
        }
        if (outlen == outcap) {
            if (outcap >= ZIM_BLOB_CAP) goto done;
            size_t ncap = outcap * 2;
            if (ncap > ZIM_BLOB_CAP) ncap = ZIM_BLOB_CAP;
            uint8_t *nb = heap_caps_realloc(outbuf, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nb) goto done;
            outbuf = nb; outcap = ncap;
        }
        ZSTD_outBuffer zout = { outbuf, outcap, outlen };
        size_t r = ZSTD_decompressStream(ds, &zout, &zin);
        outlen = zout.pos;
        if (ZSTD_isError(r)) goto done;
        if (!have_count && outlen >= (size_t)osz) {
            uint64_t off0 = (osz == 8) ? rd64(outbuf) : rd32(outbuf);
            nblobs = off0 / osz - 1;
            if (blob >= nblobs) goto done;
            have_count = true;
        }
        if (have_count && !have_range && outlen >= (size_t)(blob + 2) * osz) {
            bstart = (osz == 8) ? rd64(outbuf + (size_t)blob * osz)
                                : rd32(outbuf + (size_t)blob * osz);
            bend   = (osz == 8) ? rd64(outbuf + (size_t)(blob + 1) * osz)
                                : rd32(outbuf + (size_t)(blob + 1) * osz);
            if (bend < bstart || bend > ZIM_BLOB_CAP) goto done;
            have_range = true;
        }
        if (have_range && outlen >= bend) break;
        if (r == 0) break;
    }

    if (!have_range || outlen < bend) goto done;
    ret = (httpd_resp_send(req, (const char *)outbuf + bstart, bend - bstart) == ESP_OK)
              ? ESP_OK : ESP_ERR_INVALID_STATE;   // send fail = socket gone

done:
    if (ds) ZSTD_freeDStream(ds);
    free(inbuf);
    free(outbuf);
    return ret;
}

static esp_err_t send_uncompressed(httpd_req_t *req, zim_t *z,
                                   uint64_t data_area, int osz, uint32_t blob)
{
    uint8_t ob[16];
    size_t br;
    if (!zread(z, data_area, ob, osz, &br) || br != (size_t)osz) return ESP_FAIL;
    uint64_t off0 = (osz == 8) ? rd64(ob) : rd32(ob);
    uint64_t nblobs = off0 / osz - 1;
    if (blob >= nblobs) return ESP_FAIL;

    if (!zread(z, data_area + (uint64_t)blob * osz, ob, osz * 2, &br) || br != (size_t)osz * 2)
        return ESP_FAIL;
    uint64_t a = (osz == 8) ? rd64(ob) : rd32(ob + 0);
    uint64_t b = (osz == 8) ? rd64(ob + osz) : rd32(ob + osz);
    if (b < a) return ESP_FAIL;

    uint64_t pos = data_area + a, remain = b - a;
    char *buf = heap_caps_malloc(32768, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return ESP_ERR_NO_MEM;
    bool sent_any = false;
    while (remain > 0) {
        size_t take = remain > 32768 ? 32768 : (size_t)remain;
        if (!zread(z, pos, buf, take, &br) || br != take) {
            free(buf);
            return sent_any ? ESP_ERR_INVALID_STATE : ESP_FAIL;
        }
        if (httpd_resp_send_chunk(req, buf, take) != ESP_OK) {
            free(buf);
            return ESP_ERR_INVALID_STATE;   // client hung up
        }
        sent_any = true;
        pos += take; remain -= take;
    }
    free(buf);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t serve_blob(httpd_req_t *req, zim_t *z, dirent_z_t *d,
                            bool *cache_hit)
{
    *cache_hit = false;
    uint64_t coff, cend;
    if (!cluster_bounds(z, d->cluster, &coff, &cend)) return ESP_FAIL;

    uint8_t info;
    size_t br;
    if (!zread(z, coff, &info, 1, &br) || br != 1) return ESP_FAIL;
    int comp = info & 0x0F;
    int osz  = (info & 0x10) ? 8 : 4;

    httpd_resp_set_type(req, mime_of(z, d->mime));
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");

    if (comp <= 1) return send_uncompressed(req, z, coff + 1, osz, d->blob);

    if (comp == 5) {
        uint8_t *data = NULL;
        size_t len = 0;
        esp_err_t r = zstd_blob_copy(z, d->cluster, coff + 1, cend, osz,
                                     d->blob, &data, &len, cache_hit);
        if (r == ESP_ERR_NO_MEM)           
            return send_zstd_stream(req, z, coff + 1, cend, osz, d->blob);
        if (r != ESP_OK) return ESP_FAIL;
        r = (httpd_resp_send(req, (const char *)data, len) == ESP_OK)
                ? ESP_OK : ESP_ERR_INVALID_STATE;
        free(data);
        return r;
    }

    httpd_resp_send_err(req, 501, "This archive uses LZMA compression (only zstd is supported)");
    return ESP_OK;
}


esp_err_t zim_serve_path(httpd_req_t *req, zim_t *z, const char *rest)
{
    if (rest[0] == 0) {
        dirent_z_t d;
        if (z->mainPage == 0xFFFFFFFF || !read_dirent_by_index(z, z->mainPage, &d) ||
            !resolve_redirects(z, &d)) {
            httpd_resp_send_err(req, 404, "Archive has no main page");
            return ESP_OK;
        }
        char loc[640];
        snprintf(loc, sizeof(loc), "/infoweb/%s/%c/%s", z->name, d.ns, d.url);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", loc);
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    if (rest[1] != '/') { httpd_resp_send_err(req, 404, "Bad archive path"); return ESP_OK; }
    char ns = rest[0];
    const char *url = rest + 2;

    int64_t t0 = esp_timer_get_time();
    dirent_z_t d;
    if (!zim_find(z, ns, url, &d)) {
        httpd_resp_send_err(req, 404, "Not found in archive");
        return ESP_OK;
    }
    if (d.redirect) {
        dirent_z_t t = d;
        if (!resolve_redirects(z, &t)) { httpd_resp_send_err(req, 404, "Broken redirect"); return ESP_OK; }
        char loc[640];
        snprintf(loc, sizeof(loc), "/infoweb/%s/%c/%s", z->name, t.ns, t.url);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", loc);
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    bool hit = false;
    esp_err_t r = serve_blob(req, z, &d, &hit);
    int64_t ms = (esp_timer_get_time() - t0) / 1000;

    const char *mime = mime_of(z, d.mime);
    if (strncmp(mime, "text/html", 9) == 0)
        ESP_LOGI(TAG, "%s %c/%.60s %lldms%s", z->name, ns, url,
                 (long long)ms, hit ? " (cached)" : "");

    if (r == ESP_FAIL)
        httpd_resp_send_err(req, 500, "Archive read error");
    
    return ESP_OK;
}


static bool title_of(zim_t *z, uint32_t list_idx, dirent_z_t *d)
{
    uint8_t p[4];
    size_t br;
    if (!zread(z, z->titlePtrPos + 4ULL * list_idx, p, 4, &br) || br != 4) return false;
    return read_dirent_by_index(z, rd32(p), d);
}

int zim_search(zim_t *z, const char *prefix, zim_hit_t *hits, int max)
{
    if (z->titlePtrPos == 0 || max <= 0) return 0;
    char want_ns = (z->minor >= 1) ? 'C' : 'A';
    size_t plen = strlen(prefix);
    dirent_z_t d;

    uint32_t lo = 0, hi = z->entryCount;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (!title_of(z, mid, &d)) return 0;
        if (strcmp(d.title, prefix) < 0) lo = mid + 1;
        else hi = mid;
    }

    int n = 0;
    for (uint32_t i = lo, scanned = 0;
         i < z->entryCount && scanned < ZIM_SEARCH_SCAN_CAP && n < max;
         i++, scanned++) {
        if (!title_of(z, i, &d)) break;
        if (strncmp(d.title, prefix, plen) != 0) break;
        if (d.ns != want_ns) continue;
        strlcpy(hits[n].title, d.title, sizeof(hits[n].title));
        snprintf(hits[n].path, sizeof(hits[n].path), "%c/%s", d.ns, d.url);
        n++;
    }
    return n;
}


static bool zim_open_one(const char *fname)
{
    if (s_nzims >= ZIM_MAX_FILES) return false;
    zim_t *z = &s_zims[s_nzims];
    memset(z, 0, sizeof(*z));

    char ffpath[192];
    snprintf(ffpath, sizeof(ffpath), INFOWEB_FF_DIR "/%s", fname);

    sd_lock();
    FRESULT fr = f_open(&z->f, ffpath, FA_READ);
    sd_unlock();
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "%s: open failed (%d)", fname, fr);
        return false;
    }

    uint8_t h[80];
    size_t br;
    if (!zread(z, 0, h, 80, &br) || br != 80 || rd32(h) != 0x044D495AU) {
        ESP_LOGE(TAG, "%s: not a ZIM file", fname);
        sd_lock(); f_close(&z->f); sd_unlock();
        return false;
    }
    z->minor        = rd16(h + 6);
    z->entryCount   = rd32(h + 24);
    z->clusterCount = rd32(h + 28);
    z->urlPtrPos    = rd64(h + 32);
    z->titlePtrPos  = rd64(h + 40);
    z->clusterPtrPos= rd64(h + 48);
    z->mimeListPos  = rd64(h + 56);
    z->mainPage     = rd32(h + 64);
    z->checksumPos  = rd64(h + 72);

#if !FF_FS_EXFAT
    if (z->checksumPos > 0xFFFFFFFFULL) {
        ESP_LOGE(TAG, "%s is >4GB but exFAT is disabled — apply the ffconf.h "
                      "patch in the README, then rebuild. Skipping this file.", fname);
        sd_lock(); f_close(&z->f); sd_unlock();
        return false;
    }
#endif

    size_t cap = 4096;
    z->mimeblob = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!z->mimeblob || !zread(z, z->mimeListPos, z->mimeblob, cap - 1, &br)) {
        sd_lock(); f_close(&z->f); sd_unlock();
        return false;
    }
    z->mimeblob[br] = 0;
    size_t i = 0;
    while (i < br && z->mimeblob[i] && z->nmime < 80) {
        z->mimes[z->nmime++] = &z->mimeblob[i];
        i += strlen(&z->mimeblob[i]) + 1;
    }

    strlcpy(z->name, fname, sizeof(z->name));
    char *dot = strrchr(z->name, '.');
    if (dot) *dot = 0;
    z->id = s_nzims;
    z->open = true;
    s_nzims++;
    ESP_LOGI(TAG, "opened %s — %u entries, %u clusters, %d mime types",
             z->name, (unsigned)z->entryCount, (unsigned)z->clusterCount, z->nmime);
    return true;
}

esp_err_t zim_registry_init(void)
{
    s_cc_mtx = xSemaphoreCreateMutex();
    s_dk_mtx = xSemaphoreCreateMutex();
    s_dkeys = heap_caps_calloc(ZIM_DKEY_ENTRIES, sizeof(dkey_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_dkeys)
        ESP_LOGW(TAG, "no PSRAM for lookup cache — lookups will be slower");

    DIR *dir = opendir(INFOWEB_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "%s not found — no archives to serve", INFOWEB_DIR);
        return ESP_OK;
    }
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len > 4 && strcasecmp(e->d_name + len - 4, ".zim") == 0)
            zim_open_one(e->d_name);
    }
    closedir(dir);
    ESP_LOGI(TAG, "%d archive(s) ready, cluster cache %dMB, key cache %d entries",
             s_nzims, (int)(ZIM_CCACHE_BUDGET / (1024 * 1024)),
             s_dkeys ? ZIM_DKEY_ENTRIES : 0);
    return ESP_OK;
}

int zim_count(void) { return s_nzims; }
const char *zim_name(int i) { return (i >= 0 && i < s_nzims) ? s_zims[i].name : NULL; }

zim_t *zim_by_name(const char *name)
{
    for (int i = 0; i < s_nzims; i++)
        if (strcmp(s_zims[i].name, name) == 0) return &s_zims[i];
    return NULL;
}

zim_t *zim_at(int i) { return (i >= 0 && i < s_nzims) ? &s_zims[i] : NULL; }
