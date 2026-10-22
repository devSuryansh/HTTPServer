#include "keel/http.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static atomic_uint_fast64_t requests;
static atomic_uint_fast64_t resp_2xx;
static atomic_uint_fast64_t resp_3xx;
static atomic_uint_fast64_t resp_4xx;
static atomic_uint_fast64_t resp_5xx;
static atomic_uint_fast64_t bytes_sent;
static atomic_int active_conns;
static atomic_int metrics_ready;
static struct timespec started;

void keel_metrics_init(void) {
    if (atomic_exchange(&metrics_ready, 1) == 0) {
        clock_gettime(CLOCK_MONOTONIC, &started);
    }
}

void keel_metrics_connection(int delta) {
    keel_metrics_init();
    atomic_fetch_add(&active_conns, delta);
}

void keel_metrics_response(int status, unsigned long long nbytes) {
    keel_metrics_init();
    atomic_fetch_add(&requests, 1);
    atomic_fetch_add(&bytes_sent, nbytes);
    if (status >= 200 && status < 300) {
        atomic_fetch_add(&resp_2xx, 1);
    } else if (status >= 300 && status < 400) {
        atomic_fetch_add(&resp_3xx, 1);
    } else if (status >= 400 && status < 500) {
        atomic_fetch_add(&resp_4xx, 1);
    } else if (status >= 500 && status < 600) {
        atomic_fetch_add(&resp_5xx, 1);
    }
}

static int builtin_path(const char *path) {
    return strcmp(path, "/_keel/health") == 0 || strcmp(path, "/_keel/metrics") == 0;
}

int keel_try_builtin(const struct keel_request *req, struct keel_response *resp) {
    if (req == NULL || resp == NULL || !builtin_path(req->path)) {
        return 0;
    }
    keel_response_init(resp);
    resp->http_minor = req->http_minor;
    resp->head_only = (req->method == KEEL_METHOD_HEAD);
    resp->close_after = req->close_connection;
    if (req->http_minor >= 1 && !req->has_host) {
        keel_response_set_error(resp, 400, req->http_minor);
        return 1;
    }
    if (req->method != KEEL_METHOD_GET && req->method != KEEL_METHOD_HEAD) {
        keel_response_set_error(resp, 405, req->http_minor);
        return 1;
    }
    if (strcmp(req->path, "/_keel/health") == 0) {
        resp->status = 200;
        resp->mem = "ok\n";
        resp->mem_len = 3;
        snprintf(resp->content_type, sizeof resp->content_type, "text/plain; charset=utf-8");
        return 1;
    }

    keel_metrics_init();
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    time_t up = now.tv_sec - started.tv_sec;
    if (up < 0) {
        up = 0;
    }
    char *text = malloc(512);
    if (text == NULL) {
        keel_response_set_error(resp, 500, req->http_minor);
        return 1;
    }
    int nw = snprintf(text, 512,
                      "keel_uptime_seconds %lld\n"
                      "keel_connections_active %d\n"
                      "keel_requests_total %llu\n"
                      "keel_responses_2xx %llu\n"
                      "keel_responses_3xx %llu\n"
                      "keel_responses_4xx %llu\n"
                      "keel_responses_5xx %llu\n"
                      "keel_bytes_sent %llu\n",
                      (long long)up, atomic_load(&active_conns),
                      (unsigned long long)atomic_load(&requests), (unsigned long long)atomic_load(&resp_2xx),
                      (unsigned long long)atomic_load(&resp_3xx), (unsigned long long)atomic_load(&resp_4xx),
                      (unsigned long long)atomic_load(&resp_5xx), (unsigned long long)atomic_load(&bytes_sent));
    if (nw < 0 || nw >= 512) {
        free(text);
        keel_response_set_error(resp, 500, req->http_minor);
        return 1;
    }
    resp->status = 200;
    resp->mem = text;
    resp->mem_owned = text;
    resp->mem_len = (size_t)nw;
    snprintf(resp->content_type, sizeof resp->content_type, "text/plain; charset=utf-8");
    return 1;
}
