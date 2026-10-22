#include "keel/http.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

int keel_path_normalize(const char *decoded, char *rel, size_t rel_cap, bool *trailing_slash) {
    if (decoded == NULL || rel == NULL || rel_cap == 0 || trailing_slash == NULL) {
        return -1;
    }
    if (decoded[0] != '/') {
        return -1;
    }
    size_t len = strlen(decoded);
    bool slash = (len > 0 && decoded[len - 1] == '/');
    size_t out_len = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && decoded[i] == '/') {
            i++;
        }
        if (i >= len) {
            break;
        }
        size_t start = i;
        while (i < len && decoded[i] != '/') {
            if (decoded[i] == '\\' || decoded[i] == '\0') {
                return -1;
            }
            i++;
        }
        size_t clen = i - start;
        if (clen == 0) {
            continue;
        }
        if (clen == 1 && decoded[start] == '.') {
            continue;
        }
        if (clen == 2 && decoded[start] == '.' && decoded[start + 1] == '.') {
            if (out_len == 0) {
                return -1;
            }
            while (out_len > 0 && rel[out_len - 1] != '/') {
                out_len--;
            }
            if (out_len > 0 && rel[out_len - 1] == '/') {
                out_len--;
            }
            continue;
        }
        if (clen > 255) {
            return -1;
        }
        if (out_len > 0) {
            if (out_len + 1 >= rel_cap) {
                return -1;
            }
            rel[out_len++] = '/';
        }
        if (out_len + clen >= rel_cap) {
            return -1;
        }
        memcpy(rel + out_len, decoded + start, clen);
        out_len += clen;
    }
    rel[out_len] = '\0';
    if (out_len == 0) {
        slash = true;
    }
    *trailing_slash = slash;
    return 0;
}

static int open_nofollow_walk(int root_fd, const char *rel) {
    if (strcmp(rel, ".") == 0) {
        return openat(root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }

    char tmp[KEEL_PATH_MAX];
    size_t rel_len = strlen(rel);
    if (rel_len >= sizeof tmp) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(tmp, rel, rel_len + 1);

    int cur = openat(root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (cur < 0) {
        return -1;
    }

    char *save = NULL;
    char *tok = strtok_r(tmp, "/", &save);
    while (tok != NULL) {
        char *next = strtok_r(NULL, "/", &save);
        if (strcmp(tok, "..") == 0) {
            close(cur);
            errno = EXDEV;
            return -1;
        }
        if (strcmp(tok, ".") != 0) {
            int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
            if (next != NULL) {
                flags |= O_DIRECTORY;
            }
            int fd = openat(cur, tok, flags);
            int saved = errno;
            close(cur);
            if (fd < 0) {
                errno = saved;
                return -1;
            }
            cur = fd;
        }
        tok = next;
    }
    return cur;
}

static int open_beneath(int root_fd, const char *rel) {
    const char *path = (rel[0] == '\0') ? "." : rel;
    struct open_how how;
    memset(&how, 0, sizeof how);
    how.flags = (unsigned long long)(O_RDONLY | O_CLOEXEC | O_NOCTTY);
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
    long rc = syscall(SYS_openat2, root_fd, path, &how, sizeof how);
    if (rc >= 0) {
        return (int)rc;
    }
    if (errno == ENOSYS) {
        return open_nofollow_walk(root_fd, path);
    }
    return -1;
}

enum keel_open_status keel_open_under_root(int root_fd, const char *rel, struct keel_opened *out) {
    if (out == NULL) {
        return KEEL_OPEN_ERROR;
    }
    out->fd = -1;
    out->directory = false;
    out->size = 0;
    out->mtime = 0;
    if (rel == NULL || root_fd < 0 || rel[0] == '/' || strchr(rel, '\\') != NULL) {
        return KEEL_OPEN_FORBIDDEN;
    }

    int fd = open_beneath(root_fd, rel);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return KEEL_OPEN_NOT_FOUND;
        }
        if (errno == EACCES || errno == EPERM || errno == EXDEV || errno == ELOOP) {
            return KEEL_OPEN_FORBIDDEN;
        }
        return KEEL_OPEN_ERROR;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return KEEL_OPEN_ERROR;
    }
    out->fd = fd;
    out->size = st.st_size;
    out->mtime = st.st_mtime;
    if (S_ISDIR(st.st_mode)) {
        out->directory = true;
        return KEEL_OPEN_DIR;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        out->fd = -1;
        return KEEL_OPEN_FORBIDDEN;
    }
    return KEEL_OPEN_FILE;
}

void keel_opened_close(struct keel_opened *out) {
    if (out == NULL) {
        return;
    }
    if (out->fd >= 0) {
        close(out->fd);
        out->fd = -1;
    }
}

#define ERR_PAGE(code, text)                                                                               \
    "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><title>" code " " text               \
    "</title></head><body><h1>" code " " text "</h1></body></html>\n"

static const char *error_html(int status) {
    switch (status) {
    case 301:
        return ERR_PAGE("301", "Moved Permanently");
    case 400:
        return ERR_PAGE("400", "Bad Request");
    case 403:
        return ERR_PAGE("403", "Forbidden");
    case 404:
        return ERR_PAGE("404", "Not Found");
    case 405:
        return ERR_PAGE("405", "Method Not Allowed");
    case 408:
        return ERR_PAGE("408", "Request Timeout");
    case 414:
        return ERR_PAGE("414", "URI Too Long");
    case 416:
        return ERR_PAGE("416", "Range Not Satisfiable");
    case 417:
        return ERR_PAGE("417", "Expectation Failed");
    case 431:
        return ERR_PAGE("431", "Request Header Fields Too Large");
    case 501:
        return ERR_PAGE("501", "Not Implemented");
    case 505:
        return ERR_PAGE("505", "HTTP Version Not Supported");
    default:
        return ERR_PAGE("500", "Internal Server Error");
    }
}

void keel_response_init(struct keel_response *resp) {
    memset(resp, 0, sizeof *resp);
    resp->file_fd = -1;
    resp->status = 500;
    resp->http_minor = 1;
    resp->close_after = true;
}

void keel_response_clear(struct keel_response *resp) {
    if (resp == NULL) {
        return;
    }
    if (resp->file_fd >= 0) {
        close(resp->file_fd);
        resp->file_fd = -1;
    }
}

void keel_response_set_error(struct keel_response *resp, int status, int http_minor) {
    bool head_only = resp->head_only;
    bool close_after = resp->close_after;
    int fd = resp->file_fd;
    keel_response_init(resp);
    resp->head_only = head_only;
    resp->close_after = close_after;
    resp->http_minor = http_minor;
    if (fd >= 0) {
        close(fd);
    }
    resp->status = status;
    resp->send_allow = (status == 405);
    const char *body = error_html(status);
    resp->mem = body;
    resp->mem_len = strlen(body);
    snprintf(resp->content_type, sizeof resp->content_type, "text/html; charset=utf-8");
}

static void set_options(struct keel_response *resp, int http_minor, bool head_only) {
    bool close_after = resp->close_after;
    keel_response_init(resp);
    resp->status = 200;
    resp->http_minor = http_minor;
    resp->head_only = head_only;
    resp->close_after = close_after;
    resp->send_allow = true;
    resp->mem = "";
    resp->mem_len = 0;
    snprintf(resp->content_type, sizeof resp->content_type, "text/plain; charset=utf-8");
}

static int serve_file(struct keel_response *resp, struct keel_opened *opened, const char *rel) {
    if (opened->size < 0) {
        keel_opened_close(opened);
        keel_response_set_error(resp, 500, resp->http_minor);
        return 0;
    }
    resp->status = 200;
    resp->file_fd = opened->fd;
    opened->fd = -1;
    resp->file_size = opened->size;
    snprintf(resp->content_type, sizeof resp->content_type, "%s", keel_mime_type(rel));
    resp->mem = NULL;
    resp->mem_len = 0;
    return 0;
}

static int location_for_directory(const struct keel_request *req, char *dst, size_t cap) {
    const char *target = req->target[0] != '\0' ? req->target : req->path;
    size_t n = strlen(target);
    size_t path_end = n;
    for (size_t i = 0; i < n; i++) {
        if (target[i] == '?') {
            path_end = i;
            break;
        }
    }
    size_t query_len = n - path_end;
    if (path_end + 1 + query_len + 1 > cap) {
        return -1;
    }
    memcpy(dst, target, path_end);
    dst[path_end] = '/';
    if (query_len > 0) {
        memcpy(dst + path_end + 1, target + path_end, query_len);
    }
    dst[path_end + 1 + query_len] = '\0';
    return 0;
}

static int join_index(const char *rel, const char *index_name, char *dst, size_t cap) {
    int nw;
    if (rel[0] == '\0') {
        nw = snprintf(dst, cap, "%s", index_name);
    } else {
        nw = snprintf(dst, cap, "%s/%s", rel, index_name);
    }
    if (nw < 0 || (size_t)nw >= cap) {
        return -1;
    }
    return 0;
}

static bool index_name_ok(const char *index_name) {
    if (index_name == NULL || index_name[0] == '\0' || index_name[0] == '.') {
        return false;
    }
    if (strchr(index_name, '/') != NULL || strchr(index_name, '\\') != NULL) {
        return false;
    }
    return true;
}

int keel_prepare_static(const struct keel_request *req, int root_fd, const char *index_name,
                        struct keel_response *resp) {
    if (req == NULL || resp == NULL) {
        return -1;
    }
    keel_response_init(resp);
    resp->http_minor = req->http_minor;
    resp->head_only = (req->method == KEEL_METHOD_HEAD);
    resp->close_after = true;

    if (req->http_minor >= 1 && !req->has_host) {
        keel_response_set_error(resp, 400, req->http_minor);
        return 0;
    }
    if (req->method == KEEL_METHOD_OPTIONS) {
        set_options(resp, req->http_minor, resp->head_only);
        return 0;
    }
    if (req->asterisk) {
        keel_response_set_error(resp, 400, req->http_minor);
        return 0;
    }
    if (req->method != KEEL_METHOD_GET && req->method != KEEL_METHOD_HEAD) {
        keel_response_set_error(resp, 405, req->http_minor);
        return 0;
    }
    if (root_fd < 0 || !index_name_ok(index_name)) {
        keel_response_set_error(resp, 500, req->http_minor);
        return 0;
    }

    char rel[KEEL_PATH_MAX];
    bool slash = false;
    if (keel_path_normalize(req->path, rel, sizeof rel, &slash) != 0) {
        keel_response_set_error(resp, 400, req->http_minor);
        return 0;
    }

    struct keel_opened opened;
    enum keel_open_status st = keel_open_under_root(root_fd, rel, &opened);
    if (st == KEEL_OPEN_NOT_FOUND) {
        keel_response_set_error(resp, 404, req->http_minor);
        return 0;
    }
    if (st == KEEL_OPEN_FORBIDDEN) {
        keel_response_set_error(resp, 403, req->http_minor);
        return 0;
    }
    if (st != KEEL_OPEN_FILE && st != KEEL_OPEN_DIR) {
        keel_opened_close(&opened);
        keel_response_set_error(resp, 500, req->http_minor);
        return 0;
    }
    if (st == KEEL_OPEN_FILE) {
        if (slash) {
            keel_opened_close(&opened);
            keel_response_set_error(resp, 404, req->http_minor);
            return 0;
        }
        return serve_file(resp, &opened, rel);
    }

    keel_opened_close(&opened);
    if (!slash) {
        keel_response_set_error(resp, 301, req->http_minor);
        if (location_for_directory(req, resp->location, sizeof resp->location) != 0) {
            keel_response_set_error(resp, 500, req->http_minor);
            return 0;
        }
        resp->has_location = true;
        resp->status = 301;
        return 0;
    }

    char index_rel[KEEL_PATH_MAX];
    if (join_index(rel, index_name, index_rel, sizeof index_rel) != 0) {
        keel_response_set_error(resp, 500, req->http_minor);
        return 0;
    }
    st = keel_open_under_root(root_fd, index_rel, &opened);
    if (st == KEEL_OPEN_FILE) {
        return serve_file(resp, &opened, index_rel);
    }
    keel_opened_close(&opened);
    if (st == KEEL_OPEN_NOT_FOUND || st == KEEL_OPEN_DIR || st == KEEL_OPEN_FORBIDDEN) {
        keel_response_set_error(resp, 403, req->http_minor);
        return 0;
    }
    keel_response_set_error(resp, 500, req->http_minor);
    return 0;
}
