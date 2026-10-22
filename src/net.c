#include "keel/server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int keel_listen(const char *node, uint16_t port, struct keel_listener *out) {
    if (node == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    char portstr[8];
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    struct addrinfo *res = NULL;
    if (getaddrinfo(node, portstr, &hints, &res) != 0) {
        errno = EINVAL;
        return -1;
    }

    int fd = -1;
    int saved = EINVAL;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) {
            saved = errno;
            continue;
        }

        int one = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0) {
            saved = errno;
            close(fd);
            fd = -1;
            continue;
        }

        if (ai->ai_family == AF_INET6 && ai->ai_addrlen >= sizeof(struct sockaddr_in6)) {
            const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)ai->ai_addr;
            if (memcmp(&in6->sin6_addr, &in6addr_any, sizeof in6addr_any) == 0) {
                int v6only = 0;
                if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof v6only) != 0) {
                    saved = errno;
                    close(fd);
                    fd = -1;
                    continue;
                }
            }
        }

        if (bind(fd, ai->ai_addr, ai->ai_addrlen) != 0 || listen(fd, 128) != 0) {
            saved = errno;
            close(fd);
            fd = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        errno = saved;
        return -1;
    }

    struct sockaddr_storage ss;
    socklen_t slen = sizeof ss;
    if (getsockname(fd, (struct sockaddr *)&ss, &slen) != 0) {
        saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    int gni = getnameinfo((struct sockaddr *)&ss, slen, out->local.host, sizeof out->local.host,
                          out->local.port, sizeof out->local.port, NI_NUMERICHOST | NI_NUMERICSERV);
    if (gni != 0) {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    out->fd = fd;
    return 0;
}

int keel_accept(int listen_fd, struct keel_addr *peer) {
    struct sockaddr_storage ss;
    socklen_t slen = sizeof ss;
    int fd = accept4(listen_fd, (struct sockaddr *)&ss, &slen, SOCK_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    int one = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    if (peer != NULL) {
        int gni = getnameinfo((struct sockaddr *)&ss, slen, peer->host, sizeof peer->host, peer->port,
                              sizeof peer->port, NI_NUMERICHOST | NI_NUMERICSERV);
        if (gni != 0) {
            snprintf(peer->host, sizeof peer->host, "?");
            snprintf(peer->port, sizeof peer->port, "?");
        }
    }
    return fd;
}

void keel_listener_close(struct keel_listener *lst) {
    if (lst == NULL) {
        return;
    }
    if (lst->fd >= 0) {
        close(lst->fd);
        lst->fd = -1;
    }
}
