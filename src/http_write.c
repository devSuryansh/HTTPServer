#include "keel/http.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int keel_format_http_date(char *dst, size_t cap, time_t when) {
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm tm;
    if (dst == NULL || cap == 0 || gmtime_r(&when, &tm) == NULL) {
        return -1;
    }
    if (tm.tm_wday < 0 || tm.tm_wday > 6 || tm.tm_mon < 0 || tm.tm_mon > 11) {
        return -1;
    }
    int nw = snprintf(dst, cap, "%s, %02d %s %04d %02d:%02d:%02d GMT", days[tm.tm_wday], tm.tm_mday,
                      months[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
    if (nw < 0 || (size_t)nw >= cap) {
        return -1;
    }
    return 0;
}

struct bytebuf {
    char *data;
    size_t len;
    size_t cap;
    bool overflow;
};

static void bprintf(struct bytebuf *buf, const char *fmt, ...) {
    if (buf->overflow || buf->len >= buf->cap) {
        buf->overflow = true;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int nw = vsnprintf(buf->data + buf->len, buf->cap - buf->len, fmt, ap);
    va_end(ap);
    if (nw < 0 || (size_t)nw >= buf->cap - buf->len) {
        buf->overflow = true;
        return;
    }
    buf->len += (size_t)nw;
}

int keel_format_head(const struct keel_response *resp, char *dst, size_t cap, size_t *out_len) {
    struct bytebuf buf = {.data = dst, .len = 0, .cap = cap, .overflow = false};
    bprintf(&buf, "HTTP/1.%d %d %s\r\n", resp->http_minor, resp->status, keel_status_reason(resp->status));
    if (resp->date[0] != '\0') {
        bprintf(&buf, "Date: %s\r\n", resp->date);
    } else {
        char date[64];
        if (keel_format_http_date(date, sizeof date, time(NULL)) == 0) {
            bprintf(&buf, "Date: %s\r\n", date);
        }
    }
    if (resp->server[0] != '\0') {
        bprintf(&buf, "Server: %s\r\n", resp->server);
    }
    if (resp->content_type[0] != '\0') {
        bprintf(&buf, "Content-Type: %s\r\n", resp->content_type);
    }
    if (!resp->omit_length) {
        unsigned long long length = 0;
        if (resp->file_fd >= 0) {
            if (resp->file_size < 0) {
                return -1;
            }
            length = (unsigned long long)resp->file_size;
        } else {
            length = (unsigned long long)resp->mem_len;
        }
        bprintf(&buf, "Content-Length: %llu\r\n", length);
    }
    if (resp->etag[0] != '\0') {
        bprintf(&buf, "ETag: %s\r\n", resp->etag);
    }
    if (resp->last_modified[0] != '\0') {
        bprintf(&buf, "Last-Modified: %s\r\n", resp->last_modified);
    }
    if (resp->accept_ranges) {
        bprintf(&buf, "Accept-Ranges: bytes\r\n");
    }
    if (resp->partial) {
        bprintf(&buf, "Content-Range: bytes %llu-%llu/%llu\r\n", (unsigned long long)resp->range_start,
                (unsigned long long)resp->range_end, (unsigned long long)resp->range_total);
    } else if (resp->range_unsatisfiable) {
        bprintf(&buf, "Content-Range: bytes */%llu\r\n", (unsigned long long)resp->range_total);
    }
    bprintf(&buf, "Connection: %s\r\n", resp->close_after ? "close" : "keep-alive");
    bprintf(&buf, "X-Content-Type-Options: nosniff\r\n");
    if (resp->send_allow) {
        bprintf(&buf, "Allow: GET, HEAD, OPTIONS\r\n");
    }
    if (resp->has_location) {
        bprintf(&buf, "Location: %s\r\n", resp->location);
    }
    bprintf(&buf, "\r\n");
    if (buf.overflow) {
        return -1;
    }
    *out_len = buf.len;
    return 0;
}

static int write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

int keel_write_blocking(int fd, const struct keel_response *resp) {
    char head[4096];
    size_t head_len = 0;
    if (keel_format_head(resp, head, sizeof head, &head_len) != 0) {
        return -1;
    }
    if (write_all(fd, head, head_len) != 0) {
        return -1;
    }
    if (resp->head_only) {
        return 0;
    }
    if (resp->file_fd >= 0) {
        off_t left = resp->file_size;
        off_t pos = resp->body_off;
        char chunk[65536];
        while (left > 0) {
            size_t want = sizeof chunk;
            if ((off_t)want > left) {
                want = (size_t)left;
            }
            ssize_t n = pread(resp->file_fd, chunk, want, pos);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            if (n == 0) {
                return -1;
            }
            if (write_all(fd, chunk, (size_t)n) != 0) {
                return -1;
            }
            pos += n;
            left -= n;
        }
        return 0;
    }
    if (resp->mem != NULL && resp->mem_len > 0) {
        return write_all(fd, resp->mem, resp->mem_len);
    }
    return 0;
}
