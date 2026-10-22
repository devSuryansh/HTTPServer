#include "test_harness.h"
#include "keel/server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

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

static int read_some(int fd, char *buf, size_t cap, size_t *out) {
    size_t len = 0;
    while (len + 1 < cap) {
        ssize_t n = recv(fd, buf + len, cap - 1 - len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        len += (size_t)n;
    }
    buf[len] = '\0';
    *out = len;
    return 0;
}

static int count_token(const char *buf, const char *token) {
    int count = 0;
    const char *p = buf;
    size_t n = strlen(token);
    while ((p = strstr(p, token)) != NULL) {
        count++;
        p += n;
    }
    return count;
}

int main(void) {
    char tmpl[] = "/tmp/keel-ka-XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL);
    if (dir == NULL) {
        return keel_test_summary();
    }
    char index_path[128];
    CHECK(strlen(dir) + strlen("/index.html") < sizeof index_path);
    memcpy(index_path, dir, strlen(dir));
    memcpy(index_path + strlen(dir), "/index.html", strlen("/index.html") + 1);
    CHECK(write_file(index_path, "home\n") == 0);
    int root_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(root_fd >= 0);

    struct keel_serve_opts opts;
    keel_serve_opts_init(&opts);
    opts.header_timeout_ms = 500;
    opts.keepalive_timeout_ms = 500;
    opts.write_timeout_ms = 500;
    opts.max_requests = 100;

    const char *two = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
                      "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        int rc = keel_handle_connection(sv[1], root_fd, &opts);
        close(sv[1]);
        _exit(rc == 0 ? 0 : 1);
    }
    close(sv[1]);
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    CHECK(write_all(sv[0], two, strlen(two)) == 0);
    char response[4096];
    size_t got = 0;
    CHECK(read_some(sv[0], response, sizeof response, &got) == 0);
    shutdown(sv[0], SHUT_WR);
    CHECK(count_token(response, "HTTP/1.1 200") == 2);
    CHECK(count_token(response, "Connection: keep-alive") == 2);
    CHECK(count_token(response, "home\n") == 2);
    close(sv[0]);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    const char *closing = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
                          "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        int rc = keel_handle_connection(sv[1], root_fd, &opts);
        close(sv[1]);
        _exit(rc == 0 ? 0 : 1);
    }
    close(sv[1]);
    setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    CHECK(write_all(sv[0], closing, strlen(closing)) == 0);
    got = 0;
    CHECK(read_some(sv[0], response, sizeof response, &got) == 0);
    CHECK(count_token(response, "HTTP/1.1 200") == 1);
    CHECK(strstr(response, "Connection: close") != NULL);
    close(sv[0]);
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    opts.max_requests = 1;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        int rc = keel_handle_connection(sv[1], root_fd, &opts);
        close(sv[1]);
        _exit(rc == 0 ? 0 : 1);
    }
    close(sv[1]);
    setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    CHECK(write_all(sv[0], two, strlen(two)) == 0);
    got = 0;
    CHECK(read_some(sv[0], response, sizeof response, &got) == 0);
    CHECK(count_token(response, "HTTP/1.1 200") == 1);
    CHECK(strstr(response, "Connection: close") != NULL);
    close(sv[0]);
    CHECK(waitpid(pid, &status, 0) == pid);

    opts.max_requests = 10;
    opts.header_timeout_ms = 200;
    opts.keepalive_timeout_ms = 200;
    const char *partial = "GET / HTTP/1.1\r\nHost: localhost\r\n";
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        int rc = keel_handle_connection(sv[1], root_fd, &opts);
        close(sv[1]);
        _exit(rc == 0 ? 0 : 1);
    }
    close(sv[1]);
    tv.tv_sec = 2;
    setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    CHECK(write_all(sv[0], partial, strlen(partial)) == 0);
    got = 0;
    CHECK(read_some(sv[0], response, sizeof response, &got) == 0);
    CHECK(strstr(response, "HTTP/1.1 408") != NULL);
    close(sv[0]);
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    close(root_fd);
    unlink(index_path);
    rmdir(dir);
    return keel_test_summary();
}
