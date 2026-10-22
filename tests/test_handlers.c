#include "test_harness.h"
#include "keel/http.h"

#include <stdio.h>
#include <string.h>

static void fill(struct keel_request *req, const char *path, keel_method method) {
    memset(req, 0, sizeof *req);
    req->method = method;
    req->http_minor = 1;
    req->has_host = true;
    snprintf(req->path, sizeof req->path, "%s", path);
    snprintf(req->host, sizeof req->host, "localhost");
}

int main(void) {
    struct keel_request req;
    struct keel_response resp;

    fill(&req, "/index.html", KEEL_METHOD_GET);
    CHECK(keel_try_builtin(&req, &resp) == 0);

    fill(&req, "/_keel/health", KEEL_METHOD_GET);
    CHECK(keel_try_builtin(&req, &resp) == 1);
    CHECK(resp.status == 200);
    CHECK(resp.mem_len == 3);
    CHECK(memcmp(resp.mem, "ok\n", 3) == 0);
    keel_response_clear(&resp);

    fill(&req, "/_keel/health", KEEL_METHOD_POST);
    CHECK(keel_try_builtin(&req, &resp) == 1);
    CHECK(resp.status == 405);
    keel_response_clear(&resp);

    keel_metrics_init();
    keel_metrics_connection(2);
    keel_metrics_response(200, 10);
    keel_metrics_response(404, 0);
    fill(&req, "/_keel/metrics", KEEL_METHOD_GET);
    CHECK(keel_try_builtin(&req, &resp) == 1);
    CHECK(resp.status == 200);
    CHECK(resp.mem != NULL);
    CHECK(strstr(resp.mem, "keel_requests_total 2\n") != NULL);
    CHECK(strstr(resp.mem, "keel_responses_2xx 1\n") != NULL);
    CHECK(strstr(resp.mem, "keel_responses_4xx 1\n") != NULL);
    CHECK(strstr(resp.mem, "keel_bytes_sent 10\n") != NULL);
    CHECK(strstr(resp.mem, "keel_connections_active 2\n") != NULL);
    keel_response_clear(&resp);
    keel_metrics_connection(-2);

    return keel_test_summary();
}
