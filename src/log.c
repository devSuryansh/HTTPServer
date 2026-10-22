#include "keel/log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int access_fd = STDOUT_FILENO;
static int error_fd = STDERR_FILENO;
static int access_owned = 0;
static int error_owned = 0;
static int level = KEEL_LOG_INFO;
static int ready = 0;
static char access_path[4096];
static char error_path[4096];
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;

static int open_append(const char *path) {
    return open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}

static void set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) {
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

int keel_log_open(const char *access, const char *error, int log_level) {
    pthread_mutex_lock(&log_mu);
    level = log_level;
    if (access != NULL && access[0] != '\0') {
        int fd = open_append(access);
        if (fd < 0) {
            pthread_mutex_unlock(&log_mu);
            return -1;
        }
        if (access_owned && access_fd > STDERR_FILENO) {
            close(access_fd);
        }
        access_fd = fd;
        access_owned = 1;
        snprintf(access_path, sizeof access_path, "%s", access);
    }
    if (error != NULL && error[0] != '\0') {
        int fd = open_append(error);
        if (fd < 0) {
            pthread_mutex_unlock(&log_mu);
            return -1;
        }
        if (error_owned && error_fd > STDERR_FILENO) {
            close(error_fd);
        }
        error_fd = fd;
        error_owned = 1;
        snprintf(error_path, sizeof error_path, "%s", error);
    }
    ready = 1;
    pthread_mutex_unlock(&log_mu);
    return 0;
}

static int reopen_one(int *fd, int owned, const char *path) {
    if (!owned || path[0] == '\0') {
        return 0;
    }
    int fresh = open_append(path);
    if (fresh < 0) {
        return -1;
    }
    if (dup2(fresh, *fd) < 0) {
        close(fresh);
        return -1;
    }
    close(fresh);
    set_cloexec(*fd);
    return 0;
}

void keel_log_reopen(void) {
    pthread_mutex_lock(&log_mu);
    if (reopen_one(&access_fd, access_owned, access_path) != 0 ||
        reopen_one(&error_fd, error_owned, error_path) != 0) {
        char note[] = "keel: failed to reopen a log file\n";
        ssize_t n = write(STDERR_FILENO, note, sizeof note - 1);
        (void)n;
    }
    pthread_mutex_unlock(&log_mu);
}

void keel_log_close(void) {
    pthread_mutex_lock(&log_mu);
    if (access_owned && access_fd > STDERR_FILENO) {
        close(access_fd);
    }
    if (error_owned && error_fd > STDERR_FILENO) {
        close(error_fd);
    }
    access_fd = STDOUT_FILENO;
    error_fd = STDERR_FILENO;
    access_owned = 0;
    error_owned = 0;
    access_path[0] = '\0';
    error_path[0] = '\0';
    ready = 0;
    pthread_mutex_unlock(&log_mu);
}

static const char *level_name(int lvl) {
    switch (lvl) {
    case KEEL_LOG_ERROR:
        return "error";
    case KEEL_LOG_WARN:
        return "warn";
    case KEEL_LOG_INFO:
        return "info";
    default:
        return "debug";
    }
}

static void write_error(int lvl, const char *fmt, va_list ap) {
    if (!ready || lvl > level) {
        return;
    }
    char msg[1024];
    int nw = vsnprintf(msg, sizeof msg, fmt, ap);
    if (nw < 0) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    char stamp[64];
    if (gmtime_r(&now, &tm) == NULL) {
        snprintf(stamp, sizeof stamp, "1970-01-01T00:00:00Z");
    } else {
        snprintf(stamp, sizeof stamp, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    char line[1200];
    int total = snprintf(line, sizeof line, "%s %s %s\n", stamp, level_name(lvl), msg);
    if (total < 0) {
        return;
    }
    if ((size_t)total >= sizeof line) {
        total = (int)sizeof line - 1;
        line[total - 1] = '\n';
    }
    pthread_mutex_lock(&log_mu);
    ssize_t n = write(error_fd, line, (size_t)total);
    (void)n;
    pthread_mutex_unlock(&log_mu);
}

void keel_log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    write_error(KEEL_LOG_ERROR, fmt, ap);
    va_end(ap);
}

void keel_log_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    write_error(KEEL_LOG_WARN, fmt, ap);
    va_end(ap);
}

void keel_log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    write_error(KEEL_LOG_INFO, fmt, ap);
    va_end(ap);
}

void keel_log_debug(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    write_error(KEEL_LOG_DEBUG, fmt, ap);
    va_end(ap);
}

static void append_bytes(char *dst, size_t cap, size_t *used, const char *src, size_t n) {
    if (*used >= cap) {
        return;
    }
    if (n > cap - *used) {
        n = cap - *used;
    }
    memcpy(dst + *used, src, n);
    *used += n;
}

static void append_escaped(char *dst, size_t cap, size_t *used, const char *text) {
    if (text == NULL || text[0] == '\0') {
        append_bytes(dst, cap, used, "-", 1);
        return;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p >= 32 && *p < 127 && *p != '"' && *p != '\\') {
            char c = (char)*p;
            append_bytes(dst, cap, used, &c, 1);
        } else {
            char hex[5];
            snprintf(hex, sizeof hex, "\\x%02x", *p);
            append_bytes(dst, cap, used, hex, 4);
        }
    }
}

void keel_log_access(const char *peer, const char *request_line, int status, unsigned long long body_bytes,
                     const char *referer, const char *user_agent) {
    if (!ready) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    char zone[8] = "+0000";
    if (localtime_r(&now, &tm) != NULL) {
        strftime(zone, sizeof zone, "%z", &tm);
    } else {
        gmtime_r(&now, &tm);
    }
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int mon = (tm.tm_mon >= 0 && tm.tm_mon <= 11) ? tm.tm_mon : 0;
    char when[64];
    snprintf(when, sizeof when, "%02d/%s/%04d:%02d:%02d:%02d %s", tm.tm_mday, months[mon], tm.tm_year + 1900,
             tm.tm_hour, tm.tm_min, tm.tm_sec, zone);

    char line[2048];
    size_t used = 0;
    const char *who = (peer != NULL && peer[0] != '\0') ? peer : "-";
    char prefix[160];
    int nw = snprintf(prefix, sizeof prefix, "%s - - [%s] \"", who, when);
    if (nw < 0) {
        return;
    }
    append_bytes(line, sizeof line - 2, &used, prefix, (size_t)nw);
    append_escaped(line, sizeof line - 2, &used, request_line);
    char status_buf[64];
    if (body_bytes == 0) {
        nw = snprintf(status_buf, sizeof status_buf, "\" %d - \"", status);
    } else {
        nw = snprintf(status_buf, sizeof status_buf, "\" %d %llu \"", status, body_bytes);
    }
    if (nw < 0) {
        return;
    }
    append_bytes(line, sizeof line - 2, &used, status_buf, (size_t)nw);
    append_escaped(line, sizeof line - 2, &used, referer);
    append_bytes(line, sizeof line - 2, &used, "\" \"", 3);
    append_escaped(line, sizeof line - 2, &used, user_agent);
    append_bytes(line, sizeof line - 2, &used, "\"\n", 2);
    pthread_mutex_lock(&log_mu);
    ssize_t n = write(access_fd, line, used);
    (void)n;
    pthread_mutex_unlock(&log_mu);
}
