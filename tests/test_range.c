#include "test_harness.h"
#include "keel/http.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return -1;
    }
    size_t len = strlen(text);
    ssize_t n = write(fd, text, len);
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

static int join2(char *dst, size_t cap, const char *a, const char *b) {
    size_t na = strlen(a);
    size_t nb = strlen(b);
    if (na + 1 + nb + 1 > cap) {
        return -1;
    }
    memcpy(dst, a, na);
    dst[na] = '/';
    memcpy(dst + na + 1, b, nb + 1);
    return 0;
}

static void fill_get(struct keel_request *req, const char *target) {
    memset(req, 0, sizeof *req);
    req->method = KEEL_METHOD_GET;
    req->http_minor = 1;
    req->has_host = true;
    snprintf(req->method_token, sizeof req->method_token, "GET");
    snprintf(req->host, sizeof req->host, "localhost");
    snprintf(req->target, sizeof req->target, "%s", target);
    snprintf(req->path, sizeof req->path, "%s", target);
}

static int read_slice(const struct keel_response *resp, char *dst, size_t cap) {
    if (resp->file_fd < 0 || resp->file_size < 0 || (size_t)resp->file_size >= cap) {
        return -1;
    }
    ssize_t n = pread(resp->file_fd, dst, (size_t)resp->file_size, resp->body_off);
    if (n != resp->file_size) {
        return -1;
    }
    dst[n] = '\0';
    return 0;
}

int main(void) {
    char tmpl[] = "/tmp/keel-range-XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL);
    if (dir == NULL) {
        return keel_test_summary();
    }
    char path[128];
    CHECK(join2(path, sizeof path, dir, "digits.txt") == 0);
    CHECK(write_file(path, "0123456789") == 0);
    int root_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(root_fd >= 0);

    struct keel_request req;
    struct keel_response resp;
    char body[32];

    fill_get(&req, "/digits.txt");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 200);
    CHECK(resp.file_size == 10);
    CHECK(resp.accept_ranges == true);
    CHECK(resp.etag[0] == '"');
    CHECK(resp.last_modified[0] != '\0');
    char etag[80];
    snprintf(etag, sizeof etag, "%s", resp.etag);
    keel_response_clear(&resp);

    fill_get(&req, "/digits.txt");
    req.has_range = true;
    snprintf(req.range, sizeof req.range, "bytes=0-3");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 206);
    CHECK(resp.partial == true);
    CHECK(resp.file_size == 4);
    CHECK(resp.range_start == 0);
    CHECK(resp.range_end == 3);
    CHECK(resp.range_total == 10);
    CHECK(read_slice(&resp, body, sizeof body) == 0);
    CHECK(strcmp(body, "0123") == 0);
    keel_response_clear(&resp);

    fill_get(&req, "/digits.txt");
    req.has_range = true;
    snprintf(req.range, sizeof req.range, "bytes=-2");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 206);
    CHECK(resp.file_size == 2);
    CHECK(read_slice(&resp, body, sizeof body) == 0);
    CHECK(strcmp(body, "89") == 0);
    keel_response_clear(&resp);

    fill_get(&req, "/digits.txt");
    req.has_range = true;
    snprintf(req.range, sizeof req.range, "bytes=10-20");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 416);
    CHECK(resp.range_unsatisfiable == true);
    CHECK(resp.range_total == 10);
    keel_response_clear(&resp);

    fill_get(&req, "/digits.txt");
    req.has_range = true;
    snprintf(req.range, sizeof req.range, "bytes=0-1,2-3");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 200);
    CHECK(resp.file_size == 10);
    keel_response_clear(&resp);

    fill_get(&req, "/digits.txt");
    req.method = KEEL_METHOD_HEAD;
    req.has_range = true;
    snprintf(req.range, sizeof req.range, "bytes=2-5");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 206);
    CHECK(resp.head_only == true);
    CHECK(resp.file_size == 4);
    keel_response_clear(&resp);

    fill_get(&req, "/digits.txt");
    req.has_if_none_match = true;
    snprintf(req.if_none_match, sizeof req.if_none_match, "%s", etag);
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 304);
    CHECK(resp.omit_length == true);
    CHECK(resp.file_fd < 0);
    CHECK(strcmp(resp.etag, etag) == 0);
    keel_response_clear(&resp);

    fill_get(&req, "/digits.txt");
    req.has_if_none_match = true;
    snprintf(req.if_none_match, sizeof req.if_none_match, "*");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 304);
    keel_response_clear(&resp);

    fill_get(&req, "/digits.txt");
    req.has_if_none_match = true;
    snprintf(req.if_none_match, sizeof req.if_none_match, "W/%s", etag);
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 304);
    keel_response_clear(&resp);

    fill_get(&req, "/digits.txt");
    req.has_if_none_match = true;
    snprintf(req.if_none_match, sizeof req.if_none_match, "\"nope\"");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 200);
    keel_response_clear(&resp);

    char future[64];
    CHECK(keel_format_http_date(future, sizeof future, time(NULL) + 86400) == 0);
    fill_get(&req, "/digits.txt");
    req.has_if_modified_since = true;
    snprintf(req.if_modified_since, sizeof req.if_modified_since, "%s", future);
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 304);
    keel_response_clear(&resp);

    fill_get(&req, "/digits.txt");
    req.has_if_modified_since = true;
    snprintf(req.if_modified_since, sizeof req.if_modified_since, "Thu, 01 Jan 1970 00:00:00 GMT");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 200);
    keel_response_clear(&resp);

    close(root_fd);
    unlink(path);
    rmdir(dir);
    return keel_test_summary();
}
