#ifndef KEEL_CONFIG_H
#define KEEL_CONFIG_H

#include "keel/server.h"

#include <stdbool.h>
#include <stdint.h>

struct keel_config {
    char bind_addr[256];
    uint16_t port;
    char root[4096];
    char index_name[256];
    bool directory_listing;
    char server_name[256];
    char server_header[64];
    char access_log[4096];
    char error_log[4096];
    int log_level;
    struct keel_serve_opts serve;
};

void keel_config_init(struct keel_config *cfg);

/* 0 on success, 1 after printing help, 2 after printing the version, -1 on error. */
int keel_config_load(struct keel_config *cfg, int argc, char **argv);

#endif
