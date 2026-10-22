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
    return (n == (ssize_t)len) ? 0 : -1;
}

static void fill_get(struct keel_request *req, const char *target) {
    memset(req, 0, sizeof *req);
    req->method = KEEL_METHOD_GET;
    req->http_minor = 1;
    req->has_host = true;
    snprintf(req->method_token, sizeof req->method_token, "GET");
    snprintf(req->host, sizeof req->host, "localhost");
    snprintf(req->target, sizeof req->target, "%s", target);
    const char *q = strchr(target, '?');
    if (q == NULL) {
        snprintf(req->path, sizeof req->path, "%s", target);
    } else {
        size_t n = (size_t)(q - target);
        memcpy(req->path, target, n);
        req->path[n] = '\0';
        req->has_query = true;
        snprintf(req->query, sizeof req->query, "%s", q + 1);
    }
}

static char root_template[] = "/tmp/keel-root-XXXXXX";

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

int main(void) {
    char *parent = mkdtemp(root_template);
    CHECK(parent != NULL);
    if (parent == NULL) {
        return keel_test_summary();
    }

    char root[256];
    char secret_path[256];
    char hello[256];
    char spaced[256];
    char sub[256];
    char index_sub[256];
    char index_root[256];
    char empty[256];
    char escape[256];
    char inside[256];
    CHECK(join2(root, sizeof root, parent, "www") == 0);
    CHECK(join2(secret_path, sizeof secret_path, parent, "secret") == 0);
    CHECK(join2(hello, sizeof hello, root, "hello.txt") == 0);
    CHECK(join2(spaced, sizeof spaced, root, "hello world.txt") == 0);
    CHECK(join2(sub, sizeof sub, root, "sub") == 0);
    CHECK(join2(index_sub, sizeof index_sub, sub, "index.html") == 0);
    CHECK(join2(index_root, sizeof index_root, root, "index.html") == 0);
    CHECK(join2(empty, sizeof empty, root, "empty") == 0);
    CHECK(join2(escape, sizeof escape, root, "escape") == 0);
    CHECK(join2(inside, sizeof inside, root, "inside") == 0);

    CHECK(mkdir(root, 0755) == 0);
    CHECK(mkdir(sub, 0755) == 0);
    CHECK(mkdir(empty, 0755) == 0);
    CHECK(write_file(secret_path, "SECRET\n") == 0);
    CHECK(write_file(hello, "hello\n") == 0);
    CHECK(write_file(spaced, "space\n") == 0);
    CHECK(write_file(index_sub, "<p>idx</p>") == 0);
    CHECK(write_file(index_root, "home\n") == 0);
    CHECK(symlink("../secret", escape) == 0);
    CHECK(symlink("hello.txt", inside) == 0);

    int root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(root_fd >= 0);

    struct keel_opened opened;
    CHECK(keel_open_under_root(root_fd, "hello.txt", &opened) == KEEL_OPEN_FILE);
    CHECK(opened.size == 6);
    keel_opened_close(&opened);
    CHECK(keel_open_under_root(root_fd, "../secret", &opened) == KEEL_OPEN_FORBIDDEN);
    CHECK(keel_open_under_root(root_fd, "sub/../../secret", &opened) == KEEL_OPEN_FORBIDDEN);
    CHECK(keel_open_under_root(root_fd, "escape", &opened) == KEEL_OPEN_FORBIDDEN);
    CHECK(keel_open_under_root(root_fd, "inside", &opened) == KEEL_OPEN_FILE);
    char got[16];
    ssize_t nread = read(opened.fd, got, sizeof got);
    CHECK(nread == 6);
    CHECK(memcmp(got, "hello\n", 6) == 0);
    keel_opened_close(&opened);
    CHECK(keel_open_under_root(root_fd, "missing", &opened) == KEEL_OPEN_NOT_FOUND);

    struct keel_request req;
    struct keel_response resp;

    fill_get(&req, "/hello.txt");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 200);
    CHECK(resp.file_size == 6);
    CHECK(strcmp(resp.content_type, "text/plain; charset=utf-8") == 0);
    CHECK(resp.head_only == false);
    keel_response_clear(&resp);

    fill_get(&req, "/hello.txt");
    req.method = KEEL_METHOD_HEAD;
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 200);
    CHECK(resp.head_only == true);
    CHECK(resp.file_size == 6);
    keel_response_clear(&resp);

    fill_get(&req, "/");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 200);
    CHECK(resp.file_size == 5);
    CHECK(strcmp(resp.content_type, "text/html; charset=utf-8") == 0);
    keel_response_clear(&resp);

    fill_get(&req, "/sub");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 301);
    CHECK(resp.has_location == true);
    CHECK(strcmp(resp.location, "/sub/") == 0);
    keel_response_clear(&resp);

    fill_get(&req, "/sub/?x=1");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 200);
    CHECK(resp.file_size == 10);
    keel_response_clear(&resp);

    fill_get(&req, "/empty/");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 403);
    keel_response_clear(&resp);

    fill_get(&req, "/nope");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 404);
    keel_response_clear(&resp);

    fill_get(&req, "/../secret");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 400);
    keel_response_clear(&resp);

    fill_get(&req, "/escape");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 403);
    keel_response_clear(&resp);

    fill_get(&req, "/hello world.txt");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 200);
    CHECK(resp.file_size == 6);
    keel_response_clear(&resp);

    fill_get(&req, "/hello.txt/");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 404);
    keel_response_clear(&resp);

    fill_get(&req, "/hello.txt");
    req.has_host = false;
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 400);
    keel_response_clear(&resp);

    fill_get(&req, "/hello.txt");
    req.method = KEEL_METHOD_POST;
    snprintf(req.method_token, sizeof req.method_token, "POST");
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 405);
    CHECK(resp.send_allow == true);
    keel_response_clear(&resp);

    fill_get(&req, "*");
    req.method = KEEL_METHOD_OPTIONS;
    req.asterisk = true;
    req.path[0] = '*';
    req.path[1] = '\0';
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 200);
    CHECK(resp.send_allow == true);
    CHECK(resp.mem_len == 0);
    keel_response_clear(&resp);

    fill_get(&req, "*");
    req.asterisk = true;
    req.path[0] = '*';
    req.path[1] = '\0';
    CHECK(keel_prepare_static(&req, root_fd, "index.html", &resp) == 0);
    CHECK(resp.status == 400);
    keel_response_clear(&resp);

    close(root_fd);
    unlink(hello);
    unlink(spaced);
    unlink(index_sub);
    unlink(index_root);
    unlink(escape);
    unlink(inside);
    unlink(secret_path);
    rmdir(sub);
    rmdir(empty);
    rmdir(root);
    rmdir(parent);
    return keel_test_summary();
}
