#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

typedef struct zim_file zim_t;

typedef struct {
    char title[128];
    char path[520];   
} zim_hit_t;

esp_err_t   zim_registry_init(void);
int         zim_count(void);
const char *zim_name(int i);
zim_t      *zim_by_name(const char *name);
zim_t      *zim_at(int i);


esp_err_t zim_serve_path(httpd_req_t *req, zim_t *z, const char *rest);


int zim_search(zim_t *z, const char *prefix, zim_hit_t *hits, int max);
