#include "keel/server.h"
#include "keel/version.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [--root DIR] [--port PORT]\n", prog);
}

static int parse_port(const char *text, uint16_t *out) {
    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    unsigned long value = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        value = value * 10ul + (unsigned long)(*p - '0');
        if (value > 65535ul) {
            return -1;
        }
    }
    *out = (uint16_t)value;
    return 0;
}

int main(int argc, char **argv) {
    const char *prog = (argc > 0 && argv[0] != NULL) ? argv[0] : "keel";
    const char *root = ".";
    uint16_t port = 8080;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stdout, "usage: %s [--root DIR] [--port PORT]\n", prog);
            return 0;
        }
        if (strcmp(argv[i], "--root") == 0) {
            if (i + 1 >= argc) {
                usage(prog);
                return 2;
            }
            root = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc || parse_port(argv[++i], &port) != 0) {
                usage(prog);
                return 2;
            }
            continue;
        }
        usage(prog);
        return 2;
    }

    int root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0) {
        fprintf(stderr, "keel: root %s: %s\n", root, strerror(errno));
        return 1;
    }

    struct keel_listener lst;
    if (keel_listen("0.0.0.0", port, &lst) != 0) {
        fprintf(stderr, "keel: listen: %s\n", strerror(errno));
        close(root_fd);
        return 1;
    }

    fprintf(stderr, "keel %s: listening on %s:%s\n", KEEL_VERSION, lst.local.host, lst.local.port);

    int rc = keel_serve(lst.fd, root_fd, NULL);
    close(root_fd);
    keel_listener_close(&lst);
    if (rc != 0) {
        fprintf(stderr, "keel: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
