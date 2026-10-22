#ifndef KEEL_SERVER_H
#define KEEL_SERVER_H

#include <stdint.h>

#define KEEL_HOST_MAX 128
#define KEEL_PORT_MAX 16

struct keel_addr {
    char host[KEEL_HOST_MAX];
    char port[KEEL_PORT_MAX];
};

struct keel_listener {
    int fd;
    struct keel_addr local;
};

/* Bind and listen. port 0 selects an ephemeral port, reported in out->local. */
int keel_listen(const char *node, uint16_t port, struct keel_listener *out);

/* Accept one connection. Sets TCP_NODELAY and SO_KEEPALIVE on the result. */
int keel_accept(int listen_fd, struct keel_addr *peer);

void keel_listener_close(struct keel_listener *lst);

#endif
