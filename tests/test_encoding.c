#include "test_harness.h"
#include "keel/http.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static void fill(struct keel_request *req) {
    memset(req, 0, sizeof *req);
    req->method = KEEL_METHOD_GET;
    req->http_minor = 1;
    req->has_host = true;
    snprintf(req->path, sizeof req->path, "/app.js");
    snprintf(req->target, sizeof req->target, "/app.js");
    snprintf(req->host, sizeof req->host, "localhost");
}

int main(void) {
    char tmpl[] = "/tmp/keel-gz-XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL);
    if (dir == NULL) {
        return keel_test_summary();
    }
    char js[160];
    char gz[160];
    char errdir[160];
    char err[160];
    CHECK(join2(js, sizeof js, dir, "app.js") == 0);
    CHECK(join2(gz, sizeof gz, dir, "app.js.gz") == 0);
    CHECK(join2(errdir, sizeof errdir, dir, "errors") == 0);
    CHECK(mkdir(errdir, 0755) == 0);
    CHECK(join2(err, sizeof err, errdir, "404.html") == 0);
    CHECK(write_file(js, "plain") == 0);
    CHECK(write_file(gz, "GZIP") == 0);
    CHECK(write_file(err, "missing-page") == 0);
    int root_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(root_fd >= 0);

    struct keel_request req;
    struct keel_response resp;
    fill(&req);
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 200);
    CHECK(resp.file_size == 5);
    CHECK(resp.content_encoding[0] == '\0');
    keel_response_clear(&resp);

    fill(&req);
    req.accept_gzip = true;
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 200);
    CHECK(strcmp(resp.content_encoding, "gzip") == 0);
    CHECK(resp.file_size == 4);
    CHECK(strstr(resp.content_type, "javascript") != NULL);
    CHECK(strstr(resp.etag, "-gzip\"") != NULL);
    char body[8];
    CHECK(pread(resp.file_fd, body, 4, 0) == 4);
    CHECK(memcmp(body, "GZIP", 4) == 0);
    keel_response_clear(&resp);

    struct keel_response missing;
    keel_response_init(&missing);
    keel_response_set_error(&missing, 404, 1);
    keel_apply_error_page(&missing, root_fd);
    CHECK(missing.file_fd >= 0);
    CHECK(missing.file_size == (off_t)strlen("missing-page"));
    keel_response_clear(&missing);

    close(root_fd);
    unlink(js);
    unlink(gz);
    unlink(err);
    rmdir(errdir);
    rmdir(dir);
    return keel_test_summary();
}
