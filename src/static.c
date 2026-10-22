#include "keel/http.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/openat2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
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
    free(resp->mem_owned);
    resp->mem_owned = NULL;
}

void keel_response_set_error(struct keel_response *resp, int status, int http_minor) {
    bool head_only = resp->head_only;
    bool close_after = resp->close_after;
    int fd = resp->file_fd;
    char *owned = resp->mem_owned;
    keel_response_init(resp);
    free(owned);
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

static int month_index(const char *mon) {
    static const char *names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; i++) {
        if (strcmp(mon, names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static bool parse_http_date(const char *text, time_t *out) {
    int day = 0;
    int year = 0;
    int hour = 0;
    int min = 0;
    int sec = 0;
    char mon[4];
    if (sscanf(text, "%*[^,], %d %3s %d %d:%d:%d GMT", &day, mon, &year, &hour, &min, &sec) != 6) {
        return false;
    }
    int mi = month_index(mon);
    if (mi < 0 || day < 1 || day > 31 || hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 ||
        sec > 60) {
        return false;
    }
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_mday = day;
    tm.tm_mon = mi;
    tm.tm_year = year - 1900;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    time_t when = timegm(&tm);
    if (when == (time_t)-1) {
        return false;
    }
    *out = when;
    return true;
}

static bool etag_list_has(const char *list, const char *etag) {
    if (strcmp(list, "*") == 0) {
        return true;
    }
    const char *p = list;
    size_t etag_len = strlen(etag);
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (p[0] == 'W' && p[1] == '/') {
            p += 2;
        }
        if (*p != '"') {
            while (*p != '\0' && *p != ',') {
                p++;
            }
            continue;
        }
        const char *start = p;
        p++;
        while (*p != '\0' && *p != '"') {
            p++;
        }
        if (*p == '"') {
            p++;
        }
        size_t n = (size_t)(p - start);
        if (n == etag_len && memcmp(start, etag, n) == 0) {
            return true;
        }
    }
    return false;
}

static bool not_modified(const struct keel_request *req, const char *etag, time_t mtime) {
    if (req->has_if_none_match) {
        return etag_list_has(req->if_none_match, etag);
    }
    if (req->has_if_modified_since) {
        time_t since = 0;
        if (!parse_http_date(req->if_modified_since, &since)) {
            return false;
        }
        return mtime <= since;
    }
    return false;
}

static int parse_i64(const char *text, size_t n, long long *out) {
    if (n == 0) {
        return -1;
    }
    long long value = 0;
    for (size_t i = 0; i < n; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return -1;
        }
        int digit = text[i] - '0';
        if (value > (LLONG_MAX - digit) / 10) {
            return -1;
        }
        value = value * 10 + digit;
    }
    *out = value;
    return 0;
}

/* 0: one satisfiable range. 1: ignore the header. -1: unsatisfiable. */
static int parse_byte_range(const char *header, off_t size, off_t *start, off_t *end) {
    if (header == NULL || strncmp(header, "bytes=", 6) != 0) {
        return 1;
    }
    const char *spec = header + 6;
    if (spec[0] == '\0' || strchr(spec, ',') != NULL) {
        return 1;
    }
    if (size <= 0) {
        return -1;
    }
    if (spec[0] == '-') {
        long long suffix = 0;
        if (parse_i64(spec + 1, strlen(spec + 1), &suffix) != 0) {
            return 1;
        }
        if (suffix <= 0) {
            return -1;
        }
        if (suffix >= (long long)size) {
            *start = 0;
            *end = size - 1;
            return 0;
        }
        *start = size - (off_t)suffix;
        *end = size - 1;
        return 0;
    }
    const char *dash = strchr(spec, '-');
    if (dash == NULL) {
        return 1;
    }
    long long first = 0;
    if (parse_i64(spec, (size_t)(dash - spec), &first) != 0) {
        return 1;
    }
    if (dash[1] == '\0') {
        if (first >= (long long)size) {
            return -1;
        }
        *start = (off_t)first;
        *end = size - 1;
        return 0;
    }
    long long last = 0;
    if (parse_i64(dash + 1, strlen(dash + 1), &last) != 0) {
        return 1;
    }
    if (first > last) {
        return 1;
    }
    if (first >= (long long)size) {
        return -1;
    }
    if (last >= (long long)size) {
        last = (long long)size - 1;
    }
    *start = (off_t)first;
    *end = (off_t)last;
    return 0;
}

static void fill_validators(struct keel_response *resp, const struct keel_opened *opened) {
    snprintf(resp->etag, sizeof resp->etag, "\"%llx-%llx\"", (unsigned long long)opened->size,
             (unsigned long long)opened->mtime);
    keel_format_http_date(resp->last_modified, sizeof resp->last_modified, opened->mtime);
    resp->accept_ranges = true;
}

static int serve_file(struct keel_response *resp, const struct keel_request *req,
                      struct keel_opened *opened, const char *rel) {
    if (opened->size < 0) {
        keel_opened_close(opened);
        keel_response_set_error(resp, 500, resp->http_minor);
        return 0;
    }
    fill_validators(resp, opened);
    if (not_modified(req, resp->etag, opened->mtime)) {
        keel_opened_close(opened);
        resp->status = 304;
        resp->omit_length = true;
        resp->file_fd = -1;
        resp->file_size = 0;
        resp->mem = NULL;
        resp->mem_len = 0;
        return 0;
    }

    off_t start = 0;
    off_t end = opened->size > 0 ? opened->size - 1 : 0;
    bool partial = false;
    if (req->has_range) {
        int parsed = parse_byte_range(req->range, opened->size, &start, &end);
        if (parsed < 0) {
            off_t total = opened->size;
            char etag[80];
            char modified[64];
            snprintf(etag, sizeof etag, "%s", resp->etag);
            snprintf(modified, sizeof modified, "%s", resp->last_modified);
            keel_opened_close(opened);
            keel_response_set_error(resp, 416, resp->http_minor);
            snprintf(resp->etag, sizeof resp->etag, "%s", etag);
            snprintf(resp->last_modified, sizeof resp->last_modified, "%s", modified);
            resp->accept_ranges = true;
            resp->range_unsatisfiable = true;
            resp->range_total = total;
            return 0;
        }
        if (parsed == 0) {
            partial = true;
        }
    }

    resp->status = partial ? 206 : 200;
    resp->file_fd = opened->fd;
    opened->fd = -1;
    resp->body_off = partial ? start : 0;
    if (partial) {
        resp->file_size = end - start + 1;
        resp->partial = true;
        resp->range_start = start;
        resp->range_end = end;
        resp->range_total = opened->size;
    } else {
        resp->file_size = opened->size;
    }
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

static bool host_matches(const char *host, const char *expect) {
    if (expect == NULL || expect[0] == '\0') {
        return true;
    }
    if (host == NULL || host[0] == '\0') {
        return false;
    }
    char name[256];
    size_t n = 0;
    if (host[0] == '[') {
        const char *end = strchr(host, ']');
        if (end == NULL || end == host + 1) {
            return false;
        }
        n = (size_t)(end - host - 1);
    } else {
        const char *colon = strchr(host, ':');
        if (colon != NULL && strchr(colon + 1, ':') == NULL) {
            const char *p = colon + 1;
            int digits = *p != '\0';
            while (*p != '\0') {
                if (*p < '0' || *p > '9') {
                    digits = 0;
                    break;
                }
                p++;
            }
            n = digits ? (size_t)(colon - host) : strlen(host);
        } else {
            n = strlen(host);
        }
    }
    if (n == 0 || n >= sizeof name) {
        return false;
    }
    memcpy(name, host[0] == '[' ? host + 1 : host, n);
    name[n] = '\0';
    size_t en = strlen(expect);
    if (en != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char a = (unsigned char)name[i];
        unsigned char b = (unsigned char)expect[i];
        if (a >= 'A' && a <= 'Z') {
            a = (unsigned char)(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z') {
            b = (unsigned char)(b + ('a' - 'A'));
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

static int append_text(char **buf, size_t *len, size_t *cap, const char *text, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t next = *cap == 0 ? 4096 : *cap * 2;
        while (next < *len + n + 1) {
            next *= 2;
        }
        char *grown = realloc(*buf, next);
        if (grown == NULL) {
            return -1;
        }
        *buf = grown;
        *cap = next;
    }
    memcpy(*buf + *len, text, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

static int append_cstr(char **buf, size_t *len, size_t *cap, const char *text) {
    return append_text(buf, len, cap, text, strlen(text));
}

static int append_escaped(char **buf, size_t *len, size_t *cap, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        const char *rep = NULL;
        char one[2];
        size_t n = 1;
        if (*p == '&') {
            rep = "&amp;";
            n = 5;
        } else if (*p == '<') {
            rep = "&lt;";
            n = 4;
        } else if (*p == '>') {
            rep = "&gt;";
            n = 4;
        } else if (*p == '"') {
            rep = "&quot;";
            n = 6;
        } else if (*p < 32 || *p == 127) {
            continue;
        } else {
            one[0] = (char)*p;
            one[1] = '\0';
            rep = one;
        }
        if (append_text(buf, len, cap, rep, n) != 0) {
            return -1;
        }
    }
    return 0;
}

static int serve_listing(struct keel_response *resp, int root_fd, const char *rel,
                         const struct keel_request *req) {
    struct keel_opened opened;
    enum keel_open_status st = keel_open_under_root(root_fd, rel, &opened);
    if (st != KEEL_OPEN_DIR) {
        keel_opened_close(&opened);
        keel_response_set_error(resp, 403, req->http_minor);
        return 0;
    }
    int fd = opened.fd;
    opened.fd = -1;
    DIR *dir = fdopendir(fd);
    if (dir == NULL) {
        close(fd);
        keel_response_set_error(resp, 500, req->http_minor);
        return 0;
    }
    char *html = NULL;
    size_t len = 0;
    size_t cap = 0;
    int failed = append_cstr(&html, &len, &cap,
                             "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><title>Index of ");
    if (failed == 0) {
        failed = append_escaped(&html, &len, &cap, req->path);
    }
    if (failed == 0) {
        failed = append_cstr(&html, &len, &cap, "</title></head><body><h1>Index of ");
    }
    if (failed == 0) {
        failed = append_escaped(&html, &len, &cap, req->path);
    }
    if (failed == 0) {
        failed = append_cstr(&html, &len, &cap, "</h1><ul>\n");
    }
    int shown = 0;
    while (failed == 0) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (shown == 512) {
            failed = append_cstr(&html, &len, &cap, "<li>Further entries omitted.</li>\n");
            break;
        }
        int is_dir = entry->d_type == DT_DIR;
        if (entry->d_type == DT_UNKNOWN) {
            struct stat stbuf;
            if (fstatat(dirfd(dir), entry->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISDIR(stbuf.st_mode)) {
                is_dir = 1;
            }
        }
        failed = append_cstr(&html, &len, &cap, "<li><a href=\"");
        if (failed == 0) {
            failed = append_escaped(&html, &len, &cap, entry->d_name);
        }
        if (failed == 0 && is_dir) {
            failed = append_text(&html, &len, &cap, "/", 1);
        }
        if (failed == 0) {
            failed = append_cstr(&html, &len, &cap, "\">");
        }
        if (failed == 0) {
            failed = append_escaped(&html, &len, &cap, entry->d_name);
        }
        if (failed == 0 && is_dir) {
            failed = append_text(&html, &len, &cap, "/", 1);
        }
        if (failed == 0) {
            failed = append_cstr(&html, &len, &cap, "</a></li>\n");
        }
        shown++;
    }
    closedir(dir);
    if (failed == 0) {
        failed = append_cstr(&html, &len, &cap, "</ul></body></html>\n");
    }
    if (failed != 0 || html == NULL) {
        free(html);
        keel_response_set_error(resp, 500, req->http_minor);
        return 0;
    }
    resp->status = 200;
    resp->mem = html;
    resp->mem_owned = html;
    resp->mem_len = len;
    snprintf(resp->content_type, sizeof resp->content_type, "text/html; charset=utf-8");
    return 0;
}

int keel_prepare_site(const struct keel_request *req, int root_fd, const struct keel_site *site,
                      struct keel_response *resp) {
    if (req == NULL || resp == NULL) {
        return -1;
    }
    keel_response_init(resp);
    resp->http_minor = req->http_minor;
    resp->head_only = (req->method == KEEL_METHOD_HEAD);
    resp->close_after = req->close_connection;
    const char *index_name = (site != NULL && site->index_name != NULL) ? site->index_name : "index.html";

    if (req->http_minor >= 1 && !req->has_host) {
        keel_response_set_error(resp, 400, req->http_minor);
        return 0;
    }
    if (site != NULL && site->server_name != NULL && site->server_name[0] != '\0' &&
        !host_matches(req->has_host ? req->host : NULL, site->server_name)) {
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
        return serve_file(resp, req, &opened, rel);
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
        return serve_file(resp, req, &opened, index_rel);
    }
    keel_opened_close(&opened);
    if (st == KEEL_OPEN_NOT_FOUND && site != NULL && site->directory_listing) {
        return serve_listing(resp, root_fd, rel, req);
    }
    if (st == KEEL_OPEN_NOT_FOUND || st == KEEL_OPEN_DIR || st == KEEL_OPEN_FORBIDDEN) {
        keel_response_set_error(resp, 403, req->http_minor);
        return 0;
    }
    keel_response_set_error(resp, 500, req->http_minor);
    return 0;
}

int keel_prepare_static(const struct keel_request *req, int root_fd, const char *index_name,
                        struct keel_response *resp) {
    struct keel_site site = {
        .index_name = index_name,
        .directory_listing = false,
        .server_name = NULL,
    };
    return keel_prepare_site(req, root_fd, &site, resp);
}
