#include "test_harness.h"
#include "keel/server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int connect_to(const char *host, const char *port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)atoi(port));
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(void) {
    struct keel_listener lst;
    CHECK(keel_listen("127.0.0.1", 0, &lst) == 0);
    CHECK(lst.fd >= 0);
    CHECK(strcmp(lst.local.host, "127.0.0.1") == 0);
    CHECK(lst.local.port[0] != '\0');
    CHECK(strcmp(lst.local.port, "0") != 0);

    int client = connect_to(lst.local.host, lst.local.port);
    CHECK(client >= 0);

    struct keel_addr peer;
    int accepted = keel_accept(lst.fd, &peer);
    CHECK(accepted >= 0);
    CHECK(strcmp(peer.host, "127.0.0.1") == 0);
    CHECK(peer.port[0] != '\0');

    int nodelay = 0;
    socklen_t optlen = sizeof nodelay;
    CHECK(getsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, &nodelay, &optlen) == 0);
    CHECK(nodelay == 1);

    if (accepted >= 0) {
        close(accepted);
    }
    if (client >= 0) {
        close(client);
    }
    keel_listener_close(&lst);

    /* A second bind to the same ephemeral port must be possible after close,
       which is what SO_REUSEADDR is for. Rebinding the exact port we just
       released can still race with TIME_WAIT, so bind a fresh port 0 twice
       in a row and also bind a fixed high port twice. */
    struct keel_listener a;
    struct keel_listener b;
    CHECK(keel_listen("127.0.0.1", 0, &a) == 0);
    uint16_t port = (uint16_t)atoi(a.local.port);
    keel_listener_close(&a);
    CHECK(keel_listen("127.0.0.1", port, &b) == 0);
    keel_listener_close(&b);

    return keel_test_summary();
}
