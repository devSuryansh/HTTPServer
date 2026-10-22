#include "keel/server.h"
#include "keel/version.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *prog = (argc > 0 && argv[0] != NULL) ? argv[0] : "keel";
    (void)prog;

    struct keel_listener lst;
    if (keel_listen("0.0.0.0", 8080, &lst) != 0) {
        fprintf(stderr, "keel: listen: %s\n", strerror(errno));
        return 1;
    }

    fprintf(stderr, "keel %s: listening on %s:%s\n", KEEL_VERSION, lst.local.host, lst.local.port);

    for (;;) {
        struct keel_addr peer;
        int client = keel_accept(lst.fd, &peer);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "keel: accept: %s\n", strerror(errno));
            if (errno == EMFILE || errno == ENFILE || errno == ENOMEM || errno == ENOBUFS) {
                usleep(10000);
            }
            continue;
        }
        fprintf(stderr, "keel: accepted %s:%s\n", peer.host, peer.port);
        close(client);
    }
}
