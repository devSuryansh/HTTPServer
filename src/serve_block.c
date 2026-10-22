#include "keel/http.h"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

int keel_handle_connection(int client_fd, int root_fd) {
    char buf[8192];
    size_t len = 0;
    struct keel_parser parser;
    keel_parser_init(&parser, sizeof buf);

    for (;;) {
        size_t consumed = 0;
        keel_parse_status st = keel_parser_execute(&parser, buf, len, &consumed);
        (void)consumed;
        if (st == KEEL_PARSE_INCOMPLETE) {
            if (len == sizeof buf) {
                struct keel_response resp;
                keel_response_init(&resp);
                keel_response_set_error(&resp, 431, 1);
                int rc = keel_write_blocking(client_fd, &resp);
                keel_response_clear(&resp);
                return rc;
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
                struct keel_response resp;
                keel_response_init(&resp);
                keel_response_set_error(&resp, 400, 1);
                int rc = keel_write_blocking(client_fd, &resp);
                keel_response_clear(&resp);
                return rc;
            }
            len += (size_t)n;
            continue;
        }

        struct keel_response resp;
        keel_response_init(&resp);
        int rc;
        if (st != KEEL_PARSE_OK) {
            int status = keel_parse_to_status(st);
            if (status <= 0) {
                status = 400;
            }
            keel_response_set_error(&resp, status, 1);
            rc = keel_write_blocking(client_fd, &resp);
        } else {
            if (keel_prepare_static(&parser.req, root_fd, "index.html", &resp) != 0) {
                keel_response_set_error(&resp, 500, parser.req.http_minor);
            }
            resp.close_after = true;
            rc = keel_write_blocking(client_fd, &resp);
        }
        keel_response_clear(&resp);
        return rc;
    }
}
