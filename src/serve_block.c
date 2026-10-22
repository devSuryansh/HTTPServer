#include "keel/http.h"

#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

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

static int send_empty(int fd, int minor, int status) {
    char hdr[256];
    const char *reason = keel_status_reason(status);
    int nw = snprintf(hdr, sizeof hdr, "HTTP/1.%d %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                      minor, status, reason);
    if (nw < 0 || (size_t)nw >= sizeof hdr) {
        return -1;
    }
    return write_all(fd, hdr, (size_t)nw);
}

int keel_handle_connection(int client_fd) {
    char buf[8192];
    size_t len = 0;
    struct keel_parser parser;
    keel_parser_init(&parser, sizeof buf);

    for (;;) {
        size_t consumed = 0;
        keel_parse_status st = keel_parser_execute(&parser, buf, len, &consumed);
        if (st == KEEL_PARSE_INCOMPLETE) {
            if (len == sizeof buf) {
                return send_empty(client_fd, 1, 431);
            }
            ssize_t n = recv(client_fd, buf + len, sizeof buf - len, 0);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            if (n == 0) {
                if (len == 0) {
                    return 0;
                }
                return send_empty(client_fd, 1, 400);
            }
            len += (size_t)n;
            continue;
        }
        int status = (st == KEEL_PARSE_OK) ? 200 : keel_parse_to_status(st);
        if (status <= 0) {
            status = 400;
        }
        int minor = (st == KEEL_PARSE_OK) ? parser.req.http_minor : 1;
        return send_empty(client_fd, minor, status);
    }
}
