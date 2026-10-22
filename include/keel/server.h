#ifndef KEEL_SERVER_H
#define KEEL_SERVER_H

#include <stdbool.h>
#include <stddef.h>
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

struct keel_vhost {
    char index_name[256];
    bool directory_listing;
    char server_name[256];
    char server_header[64];
};

struct keel_serve_opts {
    int header_timeout_ms;
    int keepalive_timeout_ms;
    int write_timeout_ms;
    int max_requests;
    int max_connections;
    int workers;
    size_t max_header_bytes;
};

void keel_serve_opts_init(struct keel_serve_opts *opts);

/* Read requests and answer them from root_fd. NULL opts uses defaults.
   The caller closes client_fd. */
int keel_handle_connection(int client_fd, int root_fd, const struct keel_serve_opts *opts);

/* Accept on listen_fd until a termination signal asks the process to drain.
   site may be NULL. */
int keel_serve(int listen_fd, int root_fd, const struct keel_serve_opts *opts,
               const struct keel_vhost *site);

#endif
