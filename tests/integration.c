#include "test_harness.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_port;

static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return -1;
    }
    size_t len = strlen(text);
    ssize_t n = write(fd, text, len);
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

static int join2(char *dst, size_t cap, const char *a, const char *b) {
    size_t na = strlen(a);
    size_t nb = strlen(b);
    if (na + 1 + nb + 1 > cap) {
        return -1;
    }
    memcpy(dst, a, na);
    dst[na] = '/';
    memcpy(dst + na + 1, b, nb + 1);
    return 0;
}

static int connect_port(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const char *buf, size_t len) {
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

static int header_value(const char *headers, const char *name, char *dst, size_t cap) {
    size_t nlen = strlen(name);
    const char *p = headers;
    while (*p != '\0') {
        if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            p += nlen + 1;
            while (*p == ' ') {
                p++;
            }
            size_t n = 0;
            while (p[n] != '\0' && p[n] != '\r' && p[n] != '\n' && n + 1 < cap) {
                dst[n] = p[n];
                n++;
            }
            dst[n] = '\0';
            return 0;
        }
        const char *nl = strstr(p, "\n");
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
    return -1;
}

struct response {
    int status;
    long content_length;
    int saw_length;
    char head[4096];
    char body[8192];
    size_t body_len;
};

static int take_response(int fd, char *buf, size_t *len, size_t cap, int read_body, struct response *out) {
    memset(out, 0, sizeof *out);
    out->content_length = -1;
    for (;;) {
        char *split = NULL;
        if (*len >= 4) {
            for (size_t i = 0; i + 3 < *len; i++) {
                if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
                    split = buf + i;
                    break;
                }
            }
        }
        if (split == NULL) {
            if (*len + 1 >= cap) {
                return -1;
            }
            ssize_t n = recv(fd, buf + *len, cap - 1 - *len, 0);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            if (n == 0) {
                return -1;
            }
            *len += (size_t)n;
            buf[*len] = '\0';
            continue;
        }
        size_t head_len = (size_t)(split - buf);
        if (head_len >= sizeof out->head) {
            return -1;
        }
        memcpy(out->head, buf, head_len);
        out->head[head_len] = '\0';
        if (sscanf(out->head, "HTTP/1.%*d %d", &out->status) != 1) {
            return -1;
        }
        char cl[32];
        if (header_value(out->head, "Content-Length", cl, sizeof cl) == 0) {
            out->content_length = strtol(cl, NULL, 10);
            out->saw_length = 1;
        }
        size_t header_bytes = head_len + 4;
        size_t need = header_bytes;
        if (read_body && out->content_length > 0) {
            need += (size_t)out->content_length;
        }
        while (*len < need) {
            if (*len >= cap - 1) {
                return -1;
            }
            ssize_t n = recv(fd, buf + *len, cap - 1 - *len, 0);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            if (n == 0) {
                return -1;
            }
            *len += (size_t)n;
        }
        if (read_body && out->content_length > 0) {
            if ((size_t)out->content_length >= sizeof out->body) {
                return -1;
            }
            memcpy(out->body, buf + header_bytes, (size_t)out->content_length);
            out->body_len = (size_t)out->content_length;
            out->body[out->body_len] = '\0';
        }
        size_t rest = *len - need;
        memmove(buf, buf + need, rest);
        *len = rest;
        buf[*len] = '\0';
        return 0;
    }
}

static int exchange(int port, const char *request, int read_body, struct response *out) {
    int fd = connect_port(port);
    if (fd < 0) {
        return -1;
    }
    if (send_all(fd, request, strlen(request)) != 0) {
        close(fd);
        return -1;
    }
    char buf[16384];
    size_t len = 0;
    buf[0] = '\0';
    int rc = take_response(fd, buf, &len, sizeof buf, read_body, out);
    close(fd);
    return rc;
}

static pid_t spawn_server(const char *config, int err_fd) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        dup2(err_fd, STDERR_FILENO);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        execl("./keel", "keel", "--config", config, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static int wait_port(int err_fd, pid_t pid) {
    char buf[1024];
    size_t len = 0;
    for (int i = 0; i < 50; i++) {
        ssize_t n = read(err_fd, buf + len, sizeof buf - 1 - len);
        if (n > 0) {
            len += (size_t)n;
            buf[len] = '\0';
            char *at = strstr(buf, "listening on ");
            if (at != NULL) {
                char *line_end = strchr(at, '\n');
                char *colon = NULL;
                for (char *p = at; *p != '\0' && p != line_end; p++) {
                    if (*p == ':') {
                        colon = p;
                    }
                }
                if (colon != NULL) {
                    return atoi(colon + 1);
                }
            }
        }
        if (waitpid(pid, NULL, WNOHANG) == pid) {
            return -1;
        }
        usleep(50000);
    }
    return -1;
}

struct grab {
    int port;
    char body[64];
    int ok;
};

static void *grab_hello(void *arg) {
    struct grab *g = arg;
    struct response resp;
    const char *req = "GET /hello.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (exchange(g->port, req, 1, &resp) == 0 && resp.status == 200 && strcmp(resp.body, "hello\n") == 0) {
        g->ok = 1;
    }
    return NULL;
}

int main(void) {
    char tmpl[] = "/tmp/keel-int-XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL);
    if (dir == NULL) {
        return 1;
    }
    char index_path[256], hello[256], js[256], gz[256], empty[256], cfg[256], secret[256];
    char errdir[256], errpage[256];
    CHECK(join2(index_path, sizeof index_path, dir, "index.html") == 0);
    CHECK(join2(hello, sizeof hello, dir, "hello.txt") == 0);
    CHECK(join2(js, sizeof js, dir, "app.js") == 0);
    CHECK(join2(gz, sizeof gz, dir, "app.js.gz") == 0);
    CHECK(join2(empty, sizeof empty, dir, "empty") == 0);
    CHECK(join2(cfg, sizeof cfg, dir, "keel.conf") == 0);
    CHECK(join2(secret, sizeof secret, dir, "secret-outside") == 0);
    CHECK(write_file(index_path, "<p>home</p>\n") == 0);
    CHECK(write_file(hello, "hello\n") == 0);
    CHECK(write_file(js, "plain-js") == 0);
    CHECK(write_file(gz, "GZIPDATA") == 0);
    CHECK(write_file(secret, "SECRET") == 0);
    CHECK(mkdir(empty, 0755) == 0);
    CHECK(join2(errdir, sizeof errdir, dir, "errors") == 0);
    CHECK(mkdir(errdir, 0755) == 0);
    CHECK(join2(errpage, sizeof errpage, errdir, "404.html") == 0);
    CHECK(write_file(errpage, "custom-missing") == 0);

    char conf[1024];
    snprintf(conf, sizeof conf,
             "bind 127.0.0.1\nport 0\nroot %s\nworkers 2\n"
             "header_timeout_ms 400\nkeepalive_timeout_ms 800\n"
             "max_header_bytes 512\naccess_log %s/access.log\nerror_log %s/error.log\n",
             dir, dir, dir);
    CHECK(write_file(cfg, conf) == 0);

    int pipes[2];
    CHECK(pipe(pipes) == 0);
    pid_t pid = spawn_server(cfg, pipes[1]);
    close(pipes[1]);
    CHECK(pid > 0);
    int port = wait_port(pipes[0], pid);
    CHECK(port > 0);
    g_port = port;

    struct response resp;
    CHECK(exchange(port, "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 1, &resp) == 0);
    CHECK(resp.status == 200);
    CHECK(strcmp(resp.body, "<p>home</p>\n") == 0);
    char type[64];
    CHECK(header_value(resp.head, "Content-Type", type, sizeof type) == 0);
    CHECK(strstr(type, "text/html") != NULL);

    CHECK(exchange(port, "HEAD /hello.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 0,
                   &resp) == 0);
    CHECK(resp.status == 200);
    CHECK(resp.content_length == 6);
    CHECK(resp.body_len == 0);

    CHECK(exchange(port, "GET /no-such HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 1,
                   &resp) == 0);
    CHECK(resp.status == 404);
    CHECK(strcmp(resp.body, "custom-missing") == 0);

    CHECK(exchange(port, "GET /empty/ HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 1, &resp) ==
          0);
    CHECK(resp.status == 403);

    const char *escapes[] = {
        "GET /../secret-outside HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        "GET /%2e%2e/secret-outside HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        "GET /hello%2ftxt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        "GET /a%00b HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof escapes / sizeof escapes[0]; i++) {
        CHECK(exchange(port, escapes[i], 1, &resp) == 0);
        CHECK(resp.status == 400 || resp.status == 404);
        CHECK(strstr(resp.body, "SECRET") == NULL);
    }

    CHECK(exchange(port, "GET / HTTP/1.1\r\n\r\n", 1, &resp) == 0);
    CHECK(resp.status == 400);
    CHECK(exchange(port, "POST / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 1, &resp) == 0);
    CHECK(resp.status == 405);
    CHECK(exchange(port, "OPTIONS * HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 1, &resp) ==
          0);
    CHECK(resp.status == 200);

    int fd = connect_port(port);
    CHECK(fd >= 0);
    const char *two = "GET /hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n"
                      "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    CHECK(send_all(fd, two, strlen(two)) == 0);
    char buf[16384];
    size_t len = 0;
    buf[0] = '\0';
    CHECK(take_response(fd, buf, &len, sizeof buf, 1, &resp) == 0);
    CHECK(resp.status == 200);
    CHECK(strcmp(resp.body, "hello\n") == 0);
    CHECK(strstr(resp.head, "keep-alive") != NULL);
    struct response resp2;
    CHECK(take_response(fd, buf, &len, sizeof buf, 1, &resp2) == 0);
    CHECK(resp2.status == 200);
    CHECK(strstr(resp2.head, "Connection: close") != NULL);
    close(fd);

    CHECK(exchange(port, "GET /hello.txt HTTP/1.1\r\nHost: localhost\r\nRange: bytes=0-2\r\n"
                         "Connection: close\r\n\r\n",
                   1, &resp) == 0);
    CHECK(resp.status == 206);
    CHECK(strcmp(resp.body, "hel") == 0);
    CHECK(exchange(port, "GET /hello.txt HTTP/1.1\r\nHost: localhost\r\nRange: bytes=80-90\r\n"
                         "Connection: close\r\n\r\n",
                   1, &resp) == 0);
    CHECK(resp.status == 416);

    CHECK(exchange(port, "GET /hello.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 1,
                   &resp) == 0);
    char etag[128];
    CHECK(header_value(resp.head, "ETag", etag, sizeof etag) == 0);
    char cond[512];
    snprintf(cond, sizeof cond,
             "GET /hello.txt HTTP/1.1\r\nHost: localhost\r\nIf-None-Match: %s\r\nConnection: close\r\n\r\n",
             etag);
    CHECK(exchange(port, cond, 0, &resp) == 0);
    CHECK(resp.status == 304);

    char big[900];
    memset(big, 'A', sizeof big);
    char huge[1200];
    int nw = snprintf(huge, sizeof huge, "GET / HTTP/1.1\r\nHost: localhost\r\nX-Big: ");
    CHECK(nw > 0);
    memcpy(huge + nw, big, 600);
    memcpy(huge + nw + 600, "\r\n\r\n", 4);
    huge[nw + 604] = '\0';
    CHECK(exchange(port, huge, 1, &resp) == 0);
    CHECK(resp.status == 431);

    fd = connect_port(port);
    CHECK(fd >= 0);
    CHECK(send_all(fd, "GET / HTTP/1.1\r\nHost: localhost\r\n", 30) == 0);
    char stall[2048];
    size_t slen = 0;
    stall[0] = '\0';
    CHECK(take_response(fd, stall, &slen, sizeof stall, 1, &resp) == 0);
    CHECK(resp.status == 408);
    close(fd);

    CHECK(exchange(port, "GET /_keel/health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 1,
                   &resp) == 0);
    CHECK(resp.status == 200);
    CHECK(strcmp(resp.body, "ok\n") == 0);
    CHECK(exchange(port, "GET /_keel/metrics HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 1,
                   &resp) == 0);
    CHECK(resp.status == 200);
    CHECK(strstr(resp.body, "keel_requests_total ") != NULL);

    CHECK(exchange(port, "GET /app.js HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: gzip\r\n"
                         "Connection: close\r\n\r\n",
                   1, &resp) == 0);
    CHECK(resp.status == 200);
    CHECK(strcmp(resp.body, "GZIPDATA") == 0);
    char enc[32];
    CHECK(header_value(resp.head, "Content-Encoding", enc, sizeof enc) == 0);
    CHECK(strcmp(enc, "gzip") == 0);

    struct grab grabs[8];
    pthread_t threads[8];
    for (int i = 0; i < 8; i++) {
        grabs[i].port = port;
        grabs[i].ok = 0;
        CHECK(pthread_create(&threads[i], NULL, grab_hello, &grabs[i]) == 0);
    }
    for (int i = 0; i < 8; i++) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        CHECK(grabs[i].ok == 1);
    }

    int site_pipes[2];
    CHECK(pipe(site_pipes) == 0);
    pid_t site = fork();
    CHECK(site >= 0);
    if (site == 0) {
        dup2(site_pipes[1], STDERR_FILENO);
        execl("./keel", "keel", "--root", "examples/site", "--port", "0", "--workers", "1",
              "--access-log", "/dev/null", "--error-log", "/dev/null", (char *)NULL);
        _exit(127);
    }
    close(site_pipes[1]);
    int site_port = wait_port(site_pipes[0], site);
    CHECK(site_port > 0);
    CHECK(exchange(site_port, "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 1, &resp) ==
          0);
    CHECK(resp.status == 200);
    CHECK(strstr(resp.body, "keel") != NULL);
    CHECK(exchange(site_port, "GET /style.css HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 1,
                   &resp) == 0);
    CHECK(resp.status == 200);
    CHECK(header_value(resp.head, "Content-Type", type, sizeof type) == 0);
    CHECK(strstr(type, "text/css") != NULL);
    CHECK(exchange(site_port, "GET /mark.svg HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 1,
                   &resp) == 0);
    CHECK(resp.status == 200);
    kill(site, SIGTERM);
    waitpid(site, NULL, 0);
    close(site_pipes[0]);

    kill(pid, SIGTERM);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    close(pipes[0]);

    char access_log[256], error_log[256];
    CHECK(join2(access_log, sizeof access_log, dir, "access.log") == 0);
    CHECK(join2(error_log, sizeof error_log, dir, "error.log") == 0);
    unlink(access_log);
    unlink(error_log);
    unlink(index_path);
    unlink(hello);
    unlink(js);
    unlink(gz);
    unlink(secret);
    unlink(errpage);
    unlink(cfg);
    rmdir(empty);
    rmdir(errdir);
    rmdir(dir);
    return keel_test_summary();
}
