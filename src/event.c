#include "keel/http.h"
#include "keel/server.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { STEP_WAIT = 0, STEP_DONE = 1, STEP_MORE = 2 };
enum { CONN_READ = 0, CONN_WRITE = 1 };

#define TAG_WAKE 0xfffffffeu
#define TAG_LISTEN 0xffffffffu

struct keel_conn {
    int fd;
    int file_fd;
    int index;
    int state;
    uint32_t armed;
    bool in_use;
    bool pooled;
    bool header_started;
    bool close_after;
    bool use_file;
    int served;
    uint64_t deadline;
    char *in;
    size_t in_len;
    size_t in_cap;
    char head[4096];
    size_t head_len;
    size_t head_sent;
    off_t body_off;
    off_t body_end;
    const char *mem;
    size_t mem_len;
    size_t mem_sent;
    int32_t next_free;
    char peer_host[KEEL_HOST_MAX];
    char peer_port[KEEL_PORT_MAX];
};

struct keel_loop {
    int epfd;
    int root_fd;
    int listen_fd;
    int wake_fd;
    bool owns_sockets;
    bool accepting;
    bool draining;
    uint64_t drain_deadline;
    uint64_t accept_backoff_until;
    struct keel_serve_opts opts;
    struct keel_conn *conns;
    int capacity;
    int free_head;
    int active;
    char *arena;
    time_t date_sec;
    char date[64];
};

static int g_signal_pipe[2] = {-1, -1};
static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_hup;

static void on_signal(int sig) {
    unsigned char byte = (unsigned char)sig;
    if (sig == SIGHUP) {
        g_hup = 1;
    } else {
        g_stop = 1;
    }
    if (g_signal_pipe[1] >= 0) {
        ssize_t n = write(g_signal_pipe[1], &byte, 1);
        (void)n;
    }
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

void keel_serve_opts_init(struct keel_serve_opts *opts) {
    opts->header_timeout_ms = 5000;
    opts->keepalive_timeout_ms = 10000;
    opts->write_timeout_ms = 30000;
    opts->max_requests = 1000;
    opts->max_connections = 1024;
    opts->max_header_bytes = 8192;
}

static struct keel_serve_opts normalize_opts(const struct keel_serve_opts *in) {
    struct keel_serve_opts opts;
    if (in == NULL) {
        keel_serve_opts_init(&opts);
    } else {
        opts = *in;
    }
    if (opts.header_timeout_ms <= 0) {
        opts.header_timeout_ms = 5000;
    }
    if (opts.keepalive_timeout_ms <= 0) {
        opts.keepalive_timeout_ms = 10000;
    }
    if (opts.write_timeout_ms <= 0) {
        opts.write_timeout_ms = 30000;
    }
    if (opts.max_requests <= 0) {
        opts.max_requests = 1;
    }
    if (opts.max_connections <= 0) {
        opts.max_connections = 1024;
    }
    if (opts.max_connections > 4096) {
        opts.max_connections = 4096;
    }
    if (opts.max_header_bytes < 64) {
        opts.max_header_bytes = 64;
    }
    if (opts.max_header_bytes > 65536) {
        opts.max_header_bytes = 65536;
    }
    return opts;
}

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if ((flags & O_NONBLOCK) != 0) {
        return 0;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int arm_fd(int epfd, int fd, uint32_t *armed, uint32_t want, uint32_t tag) {
    if (*armed == want) {
        return 0;
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = want;
    ev.data.u32 = tag;
    int op = (*armed == 0) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    if (epoll_ctl(epfd, op, fd, &ev) != 0) {
        return -1;
    }
    *armed = want;
    return 0;
}

static void stamp_date(struct keel_loop *loop, struct keel_response *resp) {
    time_t sec = time(NULL);
    if (sec != loop->date_sec || loop->date[0] == '\0') {
        if (keel_format_http_date(loop->date, sizeof loop->date, sec) == 0) {
            loop->date_sec = sec;
        }
    }
    snprintf(resp->date, sizeof resp->date, "%s", loop->date);
}

static void release_conn(struct keel_loop *loop, struct keel_conn *conn) {
    if (conn->armed != 0 && conn->fd >= 0) {
        epoll_ctl(loop->epfd, EPOLL_CTL_DEL, conn->fd, NULL);
        conn->armed = 0;
    }
    if (loop->owns_sockets && conn->fd >= 0) {
        close(conn->fd);
    }
    conn->fd = -1;
    if (conn->file_fd >= 0) {
        close(conn->file_fd);
        conn->file_fd = -1;
    }
    conn->in_use = false;
    conn->in_len = 0;
    if (conn->pooled) {
        conn->next_free = loop->free_head;
        loop->free_head = conn->index;
        if (loop->active > 0) {
            loop->active--;
        }
    }
}

static int begin_write(struct keel_conn *conn, struct keel_loop *loop, struct keel_response *resp) {
    conn->close_after = resp->close_after;
    if (keel_format_head(resp, conn->head, sizeof conn->head, &conn->head_len) != 0) {
        return -1;
    }
    conn->head_sent = 0;
    conn->use_file = false;
    conn->mem = NULL;
    conn->mem_len = 0;
    conn->mem_sent = 0;
    conn->body_off = 0;
    conn->body_end = 0;
    if (conn->file_fd >= 0) {
        close(conn->file_fd);
        conn->file_fd = -1;
    }
    if (!resp->head_only && resp->file_fd >= 0 && resp->file_size > 0) {
        conn->file_fd = resp->file_fd;
        resp->file_fd = -1;
        conn->use_file = true;
        conn->body_end = resp->file_size;
    } else if (!resp->head_only && resp->mem != NULL && resp->mem_len > 0) {
        conn->mem = resp->mem;
        conn->mem_len = resp->mem_len;
    }
    if (resp->file_fd >= 0) {
        close(resp->file_fd);
        resp->file_fd = -1;
    }
    conn->state = CONN_WRITE;
    conn->deadline = mono_ms() + (uint64_t)loop->opts.write_timeout_ms;
    return 0;
}

static int queue_status(struct keel_conn *conn, struct keel_loop *loop, int status) {
    struct keel_response resp;
    keel_response_init(&resp);
    keel_response_set_error(&resp, status, 1);
    resp.close_after = true;
    stamp_date(loop, &resp);
    int rc = begin_write(conn, loop, &resp);
    keel_response_clear(&resp);
    return rc;
}

static int finish_response(struct keel_conn *conn, struct keel_loop *loop) {
    if (conn->file_fd >= 0) {
        close(conn->file_fd);
        conn->file_fd = -1;
    }
    conn->use_file = false;
    conn->mem = NULL;
    conn->mem_len = 0;
    conn->served++;
    if (conn->close_after || conn->served >= loop->opts.max_requests) {
        return STEP_DONE;
    }
    conn->state = CONN_READ;
    conn->header_started = false;
    if (conn->in_len == 0) {
        conn->deadline = mono_ms() + (uint64_t)loop->opts.keepalive_timeout_ms;
    }
    return STEP_MORE;
}

static int drop_consumed(struct keel_conn *conn, size_t consumed) {
    if (consumed > conn->in_len) {
        return -1;
    }
    if (consumed < conn->in_len) {
        memmove(conn->in, conn->in + consumed, conn->in_len - consumed);
        conn->in_len -= consumed;
    } else {
        conn->in_len = 0;
    }
    return 0;
}

static int do_read(struct keel_conn *conn, struct keel_loop *loop) {
    if (conn->in_len > 0 && !conn->header_started) {
        conn->header_started = true;
        if (conn->served > 0) {
            conn->deadline = mono_ms() + (uint64_t)loop->opts.header_timeout_ms;
        }
    }

    struct keel_parser parser;
    keel_parser_init(&parser, conn->in_cap);
    size_t consumed = 0;
    keel_parse_status st = keel_parser_execute(&parser, conn->in, conn->in_len, &consumed);
    if (st != KEEL_PARSE_INCOMPLETE) {
        if (drop_consumed(conn, consumed) != 0) {
            return STEP_DONE;
        }
        if (st != KEEL_PARSE_OK) {
            int status = keel_parse_to_status(st);
            if (status <= 0) {
                status = 400;
            }
            if (queue_status(conn, loop, status) != 0) {
                return STEP_DONE;
            }
            return STEP_MORE;
        }
        struct keel_response resp;
        if (keel_prepare_static(&parser.req, loop->root_fd, "index.html", &resp) != 0) {
            if (queue_status(conn, loop, 500) != 0) {
                return STEP_DONE;
            }
            return STEP_MORE;
        }
        stamp_date(loop, &resp);
        if (conn->served + 1 >= loop->opts.max_requests) {
            resp.close_after = true;
        }
        int rc = begin_write(conn, loop, &resp);
        keel_response_clear(&resp);
        if (rc != 0) {
            return STEP_DONE;
        }
        return STEP_MORE;
    }

    uint64_t now = mono_ms();
    if (now >= conn->deadline) {
        if (conn->in_len == 0 && conn->served > 0) {
            return STEP_DONE;
        }
        if (queue_status(conn, loop, 408) != 0) {
            return STEP_DONE;
        }
        return STEP_MORE;
    }
    if (conn->in_len >= conn->in_cap) {
        if (queue_status(conn, loop, 431) != 0) {
            return STEP_DONE;
        }
        return STEP_MORE;
    }

    ssize_t n = recv(conn->fd, conn->in + conn->in_len, conn->in_cap - conn->in_len, 0);
    if (n < 0) {
        if (errno == EINTR) {
            return STEP_MORE;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return STEP_WAIT;
        }
        return STEP_DONE;
    }
    if (n == 0) {
        if (conn->in_len == 0) {
            return STEP_DONE;
        }
        if (queue_status(conn, loop, 400) != 0) {
            return STEP_DONE;
        }
        return STEP_MORE;
    }
    conn->in_len += (size_t)n;
    if (!conn->header_started) {
        conn->header_started = true;
        if (conn->served > 0) {
            conn->deadline = mono_ms() + (uint64_t)loop->opts.header_timeout_ms;
        }
    }
    return STEP_MORE;
}

static int do_write(struct keel_conn *conn, struct keel_loop *loop) {
    if (mono_ms() >= conn->deadline) {
        return STEP_DONE;
    }
    if (conn->head_sent < conn->head_len) {
        ssize_t n = send(conn->fd, conn->head + conn->head_sent, conn->head_len - conn->head_sent,
                         MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                return STEP_MORE;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return STEP_WAIT;
            }
            return STEP_DONE;
        }
        if (n == 0) {
            return STEP_DONE;
        }
        conn->head_sent += (size_t)n;
        conn->deadline = mono_ms() + (uint64_t)loop->opts.write_timeout_ms;
        return STEP_MORE;
    }
    if (conn->use_file && conn->body_off < conn->body_end) {
        off_t remain = conn->body_end - conn->body_off;
        size_t count = remain > (off_t)(1 << 20) ? (size_t)(1 << 20) : (size_t)remain;
        ssize_t n = sendfile(conn->fd, conn->file_fd, &conn->body_off, count);
        if (n < 0) {
            if (errno == EINTR) {
                return STEP_MORE;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return STEP_WAIT;
            }
            return STEP_DONE;
        }
        if (n == 0) {
            return STEP_DONE;
        }
        conn->deadline = mono_ms() + (uint64_t)loop->opts.write_timeout_ms;
        return STEP_MORE;
    }
    if (conn->mem != NULL && conn->mem_sent < conn->mem_len) {
        ssize_t n = send(conn->fd, conn->mem + conn->mem_sent, conn->mem_len - conn->mem_sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                return STEP_MORE;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return STEP_WAIT;
            }
            return STEP_DONE;
        }
        if (n == 0) {
            return STEP_DONE;
        }
        conn->mem_sent += (size_t)n;
        conn->deadline = mono_ms() + (uint64_t)loop->opts.write_timeout_ms;
        return STEP_MORE;
    }
    return finish_response(conn, loop);
}

static int conn_progress(struct keel_loop *loop, struct keel_conn *conn) {
    for (;;) {
        int step = (conn->state == CONN_READ) ? do_read(conn, loop) : do_write(conn, loop);
        if (step == STEP_MORE) {
            continue;
        }
        if (step == STEP_DONE) {
            release_conn(loop, conn);
            return STEP_DONE;
        }
        uint32_t want = (conn->state == CONN_READ) ? EPOLLIN : EPOLLOUT;
        uint32_t tag = conn->pooled ? (uint32_t)conn->index : 0;
        if (arm_fd(loop->epfd, conn->fd, &conn->armed, want, tag) != 0) {
            release_conn(loop, conn);
            return STEP_DONE;
        }
        return STEP_WAIT;
    }
}

static void conn_reset_new(struct keel_conn *conn, int fd, const struct keel_addr *peer, size_t cap,
                           uint64_t deadline) {
    conn->fd = fd;
    conn->file_fd = -1;
    conn->state = CONN_READ;
    conn->armed = 0;
    conn->in_use = true;
    conn->header_started = false;
    conn->close_after = false;
    conn->use_file = false;
    conn->served = 0;
    conn->deadline = deadline;
    conn->in_len = 0;
    conn->in_cap = cap;
    conn->head_len = 0;
    conn->head_sent = 0;
    conn->mem = NULL;
    conn->mem_len = 0;
    conn->mem_sent = 0;
    if (peer != NULL) {
        snprintf(conn->peer_host, sizeof conn->peer_host, "%s", peer->host);
        snprintf(conn->peer_port, sizeof conn->peer_port, "%s", peer->port);
    } else {
        conn->peer_host[0] = '\0';
        conn->peer_port[0] = '\0';
    }
}

static int next_timeout(struct keel_loop *loop, uint64_t now) {
    uint64_t next = UINT64_MAX;
    for (int i = 0; i < loop->capacity; i++) {
        if (loop->conns[i].in_use && loop->conns[i].deadline < next) {
            next = loop->conns[i].deadline;
        }
    }
    if (loop->draining && loop->drain_deadline < next) {
        next = loop->drain_deadline;
    }
    if (!loop->accepting && loop->accept_backoff_until != 0 && loop->accept_backoff_until < next) {
        next = loop->accept_backoff_until;
    }
    if (next == UINT64_MAX) {
        return -1;
    }
    if (next <= now) {
        return 0;
    }
    uint64_t delta = next - now;
    if (delta > 60000ull) {
        return 60000;
    }
    return (int)delta;
}

static void sweep_deadlines(struct keel_loop *loop) {
    uint64_t now = mono_ms();
    for (int i = 0; i < loop->capacity; i++) {
        struct keel_conn *conn = &loop->conns[i];
        if (!conn->in_use || now < conn->deadline) {
            continue;
        }
        conn_progress(loop, conn);
    }
    if (loop->draining && now >= loop->drain_deadline) {
        for (int i = 0; i < loop->capacity; i++) {
            if (loop->conns[i].in_use) {
                release_conn(loop, &loop->conns[i]);
            }
        }
    }
}

static void start_drain(struct keel_loop *loop) {
    if (loop->draining) {
        return;
    }
    loop->draining = true;
    loop->accepting = false;
    loop->drain_deadline = mono_ms() + 5000ull;
    if (loop->listen_fd >= 0) {
        epoll_ctl(loop->epfd, EPOLL_CTL_DEL, loop->listen_fd, NULL);
    }
    for (int i = 0; i < loop->capacity; i++) {
        struct keel_conn *conn = &loop->conns[i];
        if (conn->in_use && conn->state == CONN_READ && conn->in_len == 0) {
            release_conn(loop, conn);
        }
    }
}

static int add_listen(struct keel_loop *loop) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    ev.data.u32 = TAG_LISTEN;
    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, loop->listen_fd, &ev) != 0) {
        return -1;
    }
    loop->accepting = true;
    return 0;
}

static void pause_accept(struct keel_loop *loop, uint64_t ms) {
    if (loop->accepting && loop->listen_fd >= 0) {
        epoll_ctl(loop->epfd, EPOLL_CTL_DEL, loop->listen_fd, NULL);
    }
    loop->accepting = false;
    loop->accept_backoff_until = mono_ms() + ms;
}

static void accept_ready(struct keel_loop *loop) {
    if (!loop->accepting || loop->draining) {
        return;
    }
    for (;;) {
        if (loop->free_head < 0) {
            struct keel_addr peer;
            int fd = keel_accept(loop->listen_fd, &peer);
            if (fd < 0) {
                return;
            }
            static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                                       "Connection: close\r\nRetry-After: 1\r\n\r\n";
            ssize_t n = send(fd, busy, sizeof busy - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
            (void)n;
            close(fd);
            continue;
        }
        struct keel_addr peer;
        int fd = keel_accept(loop->listen_fd, &peer);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EMFILE || errno == ENFILE || errno == ENOMEM || errno == ENOBUFS) {
                pause_accept(loop, 20);
            }
            return;
        }
        if (set_nonblock(fd) != 0) {
            close(fd);
            continue;
        }
        int index = loop->free_head;
        struct keel_conn *conn = &loop->conns[index];
        loop->free_head = conn->next_free;
        conn_reset_new(conn, fd, &peer, loop->opts.max_header_bytes,
                       mono_ms() + (uint64_t)loop->opts.header_timeout_ms);
        conn->pooled = true;
        conn->index = index;
        loop->active++;
        fprintf(stderr, "keel: accepted %s:%s\n", conn->peer_host, conn->peer_port);
        conn_progress(loop, conn);
    }
}

static void init_conn_storage(struct keel_conn *conn, int index, char *buf, size_t cap) {
    memset(conn, 0, sizeof *conn);
    conn->fd = -1;
    conn->file_fd = -1;
    conn->index = index;
    conn->in = buf;
    conn->in_cap = cap;
    conn->next_free = index + 1;
}

static int setup_slab(struct keel_loop *loop) {
    int cap = loop->opts.max_connections;
    size_t header = loop->opts.max_header_bytes;
    if (header != 0 && (size_t)cap > SIZE_MAX / header) {
        errno = ENOMEM;
        return -1;
    }
    loop->conns = calloc((size_t)cap, sizeof(struct keel_conn));
    loop->arena = malloc((size_t)cap * header);
    if (loop->conns == NULL || loop->arena == NULL) {
        free(loop->conns);
        free(loop->arena);
        loop->conns = NULL;
        loop->arena = NULL;
        errno = ENOMEM;
        return -1;
    }
    for (int i = 0; i < cap; i++) {
        init_conn_storage(&loop->conns[i], i, loop->arena + (size_t)i * header, header);
    }
    loop->conns[cap - 1].next_free = -1;
    loop->free_head = 0;
    loop->capacity = cap;
    loop->active = 0;
    return 0;
}

static void destroy_loop(struct keel_loop *loop) {
    if (loop->conns != NULL) {
        for (int i = 0; i < loop->capacity; i++) {
            if (loop->conns[i].in_use) {
                release_conn(loop, &loop->conns[i]);
            }
        }
    }
    free(loop->conns);
    free(loop->arena);
    if (loop->epfd >= 0) {
        close(loop->epfd);
    }
}

int keel_handle_connection(int client_fd, int root_fd, const struct keel_serve_opts *opts_in) {
    if (set_nonblock(client_fd) != 0) {
        return -1;
    }
    struct keel_loop loop;
    memset(&loop, 0, sizeof loop);
    loop.epfd = -1;
    loop.listen_fd = -1;
    loop.wake_fd = -1;
    loop.root_fd = root_fd;
    loop.owns_sockets = false;
    loop.opts = normalize_opts(opts_in);
    loop.capacity = 1;
    loop.date_sec = 0;

    struct keel_conn conn;
    char *buf = malloc(loop.opts.max_header_bytes);
    if (buf == NULL) {
        return -1;
    }
    init_conn_storage(&conn, 0, buf, loop.opts.max_header_bytes);
    conn_reset_new(&conn, client_fd, NULL, loop.opts.max_header_bytes,
                   mono_ms() + (uint64_t)loop.opts.header_timeout_ms);
    conn.pooled = false;
    loop.conns = &conn;
    loop.epfd = epoll_create1(EPOLL_CLOEXEC);
    if (loop.epfd < 0) {
        free(buf);
        return -1;
    }
    if (conn_progress(&loop, &conn) == STEP_WAIT) {
        while (conn.in_use) {
            int timeout = 0;
            uint64_t now = mono_ms();
            if (conn.deadline > now) {
                uint64_t delta = conn.deadline - now;
                timeout = delta > 60000ull ? 60000 : (int)delta;
            }
            struct epoll_event ev;
            memset(&ev, 0, sizeof ev);
            int n = epoll_wait(loop.epfd, &ev, 1, timeout);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                release_conn(&loop, &conn);
                break;
            }
            conn_progress(&loop, &conn);
        }
    }
    int failed = conn.in_use ? -1 : 0;
    if (conn.in_use) {
        release_conn(&loop, &conn);
    }
    close(loop.epfd);
    free(buf);
    return failed;
}

int keel_serve(int listen_fd, int root_fd, const struct keel_serve_opts *opts_in) {
    if (set_nonblock(listen_fd) != 0) {
        return -1;
    }
    struct keel_loop loop;
    memset(&loop, 0, sizeof loop);
    loop.epfd = -1;
    loop.listen_fd = listen_fd;
    loop.wake_fd = -1;
    loop.root_fd = root_fd;
    loop.owns_sockets = true;
    loop.opts = normalize_opts(opts_in);
    if (setup_slab(&loop) != 0) {
        return -1;
    }
    loop.epfd = epoll_create1(EPOLL_CLOEXEC);
    if (loop.epfd < 0) {
        destroy_loop(&loop);
        return -1;
    }
    if (pipe2(g_signal_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        destroy_loop(&loop);
        return -1;
    }
    loop.wake_fd = g_signal_pipe[0];
    struct epoll_event wake;
    memset(&wake, 0, sizeof wake);
    wake.events = EPOLLIN;
    wake.data.u32 = TAG_WAKE;
    if (epoll_ctl(loop.epfd, EPOLL_CTL_ADD, loop.wake_fd, &wake) != 0 || add_listen(&loop) != 0) {
        close(g_signal_pipe[0]);
        close(g_signal_pipe[1]);
        g_signal_pipe[0] = -1;
        g_signal_pipe[1] = -1;
        destroy_loop(&loop);
        return -1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    g_stop = 0;
    g_hup = 0;

    while (!loop.draining || loop.active > 0) {
        if (g_stop && !loop.draining) {
            start_drain(&loop);
        }
        if (g_hup) {
            g_hup = 0;
        }
        uint64_t now = mono_ms();
        if (!loop.draining && !loop.accepting && loop.accept_backoff_until != 0 &&
            now >= loop.accept_backoff_until) {
            loop.accept_backoff_until = 0;
            if (add_listen(&loop) != 0) {
                pause_accept(&loop, 50);
            }
        }
        int timeout = next_timeout(&loop, now);
        struct epoll_event events[64];
        int n = epoll_wait(loop.epfd, events, 64, timeout);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (int i = 0; i < n; i++) {
            uint32_t tag = events[i].data.u32;
            if (tag == TAG_WAKE) {
                unsigned char scratch[32];
                while (read(loop.wake_fd, scratch, sizeof scratch) > 0) {
                }
                continue;
            }
            if (tag == TAG_LISTEN) {
                accept_ready(&loop);
                continue;
            }
            if (tag >= (uint32_t)loop.capacity) {
                continue;
            }
            struct keel_conn *conn = &loop.conns[tag];
            if (!conn->in_use) {
                continue;
            }
            if ((events[i].events & (EPOLLERR | EPOLLHUP)) != 0 &&
                (events[i].events & (EPOLLIN | EPOLLOUT)) == 0) {
                release_conn(&loop, conn);
                continue;
            }
            conn_progress(&loop, conn);
        }
        sweep_deadlines(&loop);
        if (loop.draining && loop.active == 0) {
            break;
        }
        if (loop.draining && mono_ms() >= loop.drain_deadline) {
            break;
        }
    }

    destroy_loop(&loop);
    close(g_signal_pipe[0]);
    close(g_signal_pipe[1]);
    g_signal_pipe[0] = -1;
    g_signal_pipe[1] = -1;
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    return 0;
}
