#include "keel/config.h"
#include "keel/version.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void keel_config_init(struct keel_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    snprintf(cfg->bind_addr, sizeof cfg->bind_addr, "0.0.0.0");
    cfg->port = 8080;
    snprintf(cfg->root, sizeof cfg->root, ".");
    snprintf(cfg->index_name, sizeof cfg->index_name, "index.html");
    cfg->directory_listing = false;
    snprintf(cfg->server_header, sizeof cfg->server_header, "keel");
    cfg->log_level = 2;
    keel_serve_opts_init(&cfg->serve);
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [--config FILE] [--bind ADDR] [--port PORT] [--root DIR]\n"
            "          [--workers N] [--access-log PATH] [--error-log PATH]\n",
            prog);
}

static int eq_i(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (tolower(ca) != tolower(cb)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int copy_bounded(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) {
        return -1;
    }
    memcpy(dst, src, n + 1);
    return 0;
}

static int parse_long(const char *text, long min, long max, long *out) {
    if (text == NULL || text[0] == '\0' || text[0] == '+' || text[0] == '-') {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < min || value > max) {
        return -1;
    }
    *out = value;
    return 0;
}

static int parse_bool(const char *text, bool *out) {
    if (eq_i(text, "on") || eq_i(text, "true") || eq_i(text, "yes") || eq_i(text, "1")) {
        *out = true;
        return 0;
    }
    if (eq_i(text, "off") || eq_i(text, "false") || eq_i(text, "no") || eq_i(text, "0")) {
        *out = false;
        return 0;
    }
    return -1;
}

static int index_ok(const char *name) {
    if (name[0] == '\0' || name[0] == '.' || strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        return 0;
    }
    return 1;
}

static int apply_key(struct keel_config *cfg, const char *key, const char *value) {
    long number = 0;
    if (strcmp(key, "bind") == 0) {
        return copy_bounded(cfg->bind_addr, sizeof cfg->bind_addr, value);
    }
    if (strcmp(key, "port") == 0) {
        if (parse_long(value, 0, 65535, &number) != 0) {
            return -1;
        }
        cfg->port = (uint16_t)number;
        return 0;
    }
    if (strcmp(key, "root") == 0) {
        return copy_bounded(cfg->root, sizeof cfg->root, value);
    }
    if (strcmp(key, "workers") == 0) {
        if (parse_long(value, 0, 64, &number) != 0) {
            return -1;
        }
        cfg->serve.workers = (int)number;
        return 0;
    }
    if (strcmp(key, "max_connections") == 0) {
        if (parse_long(value, 1, 4096, &number) != 0) {
            return -1;
        }
        cfg->serve.max_connections = (int)number;
        return 0;
    }
    if (strcmp(key, "header_timeout_ms") == 0) {
        if (parse_long(value, 1, 600000, &number) != 0) {
            return -1;
        }
        cfg->serve.header_timeout_ms = (int)number;
        return 0;
    }
    if (strcmp(key, "keepalive_timeout_ms") == 0) {
        if (parse_long(value, 1, 600000, &number) != 0) {
            return -1;
        }
        cfg->serve.keepalive_timeout_ms = (int)number;
        return 0;
    }
    if (strcmp(key, "write_timeout_ms") == 0) {
        if (parse_long(value, 1, 600000, &number) != 0) {
            return -1;
        }
        cfg->serve.write_timeout_ms = (int)number;
        return 0;
    }
    if (strcmp(key, "max_header_bytes") == 0) {
        if (parse_long(value, 64, 65536, &number) != 0) {
            return -1;
        }
        cfg->serve.max_header_bytes = (size_t)number;
        return 0;
    }
    if (strcmp(key, "max_requests_per_conn") == 0) {
        if (parse_long(value, 1, 1000000, &number) != 0) {
            return -1;
        }
        cfg->serve.max_requests = (int)number;
        return 0;
    }
    if (strcmp(key, "index") == 0) {
        if (!index_ok(value) || copy_bounded(cfg->index_name, sizeof cfg->index_name, value) != 0) {
            return -1;
        }
        return 0;
    }
    if (strcmp(key, "directory_listing") == 0) {
        return parse_bool(value, &cfg->directory_listing);
    }
    if (strcmp(key, "server_name") == 0) {
        return copy_bounded(cfg->server_name, sizeof cfg->server_name, value);
    }
    if (strcmp(key, "access_log") == 0) {
        return copy_bounded(cfg->access_log, sizeof cfg->access_log, value);
    }
    if (strcmp(key, "error_log") == 0) {
        return copy_bounded(cfg->error_log, sizeof cfg->error_log, value);
    }
    if (strcmp(key, "server_header") == 0) {
        return copy_bounded(cfg->server_header, sizeof cfg->server_header, value);
    }
    if (strcmp(key, "log_level") == 0) {
        if (eq_i(value, "error")) {
            cfg->log_level = 0;
        } else if (eq_i(value, "warn")) {
            cfg->log_level = 1;
        } else if (eq_i(value, "info")) {
            cfg->log_level = 2;
        } else if (eq_i(value, "debug")) {
            cfg->log_level = 3;
        } else {
            return -1;
        }
        return 0;
    }
    return -1;
}

static int unquote(const char *src, char *dst, size_t cap) {
    size_t n = 0;
    if (src[0] != '"') {
        return copy_bounded(dst, cap, src);
    }
    for (size_t i = 1; src[i] != '\0'; i++) {
        char c = src[i];
        if (c == '"') {
            if (src[i + 1] != '\0') {
                return -1;
            }
            if (n >= cap) {
                return -1;
            }
            dst[n] = '\0';
            return 0;
        }
        if (c == '\\') {
            char next = src[++i];
            if (next != '\\' && next != '"') {
                return -1;
            }
            c = next;
        }
        if (n + 1 >= cap) {
            return -1;
        }
        dst[n++] = c;
    }
    return -1;
}

static int apply_line(struct keel_config *cfg, char *line) {
    char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0' || *p == '#') {
        return 0;
    }
    char *key = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        p++;
    }
    if (*p == '\0') {
        return -1;
    }
    *p++ = '\0';
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        return -1;
    }
    size_t len = strlen(p);
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\r')) {
        p[--len] = '\0';
    }
    char value[4096];
    if (unquote(p, value, sizeof value) != 0) {
        return -1;
    }
    return apply_key(cfg, key, value);
}

static int load_file(struct keel_config *cfg, const char *path) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "keel: config %s: %s\n", path, strerror(errno));
        return -1;
    }
    char line[8192];
    int rc = 0;
    while (fgets(line, sizeof line, fp) != NULL) {
        size_t n = strlen(line);
        if (n == sizeof line - 1 && line[n - 1] != '\n') {
            fprintf(stderr, "keel: config line too long\n");
            rc = -1;
            break;
        }
        if (n > 0 && line[n - 1] == '\n') {
            line[n - 1] = '\0';
        }
        if (apply_line(cfg, line) != 0) {
            fprintf(stderr, "keel: invalid config line\n");
            rc = -1;
            break;
        }
    }
    fclose(fp);
    return rc;
}

static int take_arg(int argc, char **argv, int *index, const char **out) {
    if (*index + 1 >= argc) {
        return -1;
    }
    *index += 1;
    *out = argv[*index];
    return 0;
}

int keel_config_load(struct keel_config *cfg, int argc, char **argv) {
    const char *prog = (argc > 0 && argv != NULL && argv[0] != NULL) ? argv[0] : "keel";
    keel_config_init(cfg);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(prog);
            return 1;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("keel %s\n", KEEL_VERSION);
            return 2;
        }
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            const char *path = NULL;
            if (take_arg(argc, argv, &i, &path) != 0 || load_file(cfg, path) != 0) {
                if (path == NULL) {
                    usage(prog);
                }
                return -1;
            }
        }
    }
    for (int i = 1; i < argc; i++) {
        const char *value = NULL;
        if (strcmp(argv[i], "--config") == 0) {
            i++;
            continue;
        }
        if (strcmp(argv[i], "--bind") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0 || apply_key(cfg, "bind", value) != 0) {
                usage(prog);
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--port") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0 || apply_key(cfg, "port", value) != 0) {
                usage(prog);
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--root") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0 || apply_key(cfg, "root", value) != 0) {
                usage(prog);
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--workers") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0 || apply_key(cfg, "workers", value) != 0) {
                usage(prog);
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--access-log") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0 || apply_key(cfg, "access_log", value) != 0) {
                usage(prog);
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--error-log") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0 || apply_key(cfg, "error_log", value) != 0) {
                usage(prog);
                return -1;
            }
            continue;
        }
        usage(prog);
        return -1;
    }
    return 0;
}
