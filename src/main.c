#include "keel/config.h"
#include "keel/log.h"
#include "keel/server.h"
#include "keel/version.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void fill_vhost(struct keel_vhost *site, const struct keel_config *cfg) {
    snprintf(site->index_name, sizeof site->index_name, "%s", cfg->index_name);
    site->directory_listing = cfg->directory_listing;
    snprintf(site->server_name, sizeof site->server_name, "%s", cfg->server_name);
    snprintf(site->server_header, sizeof site->server_header, "%s", cfg->server_header);
}

int main(int argc, char **argv) {
    struct keel_config cfg;
    int loaded = keel_config_load(&cfg, argc, argv);
    if (loaded == 1 || loaded == 2) {
        return 0;
    }
    if (loaded != 0) {
        return 2;
    }
    if (keel_log_open(cfg.access_log, cfg.error_log, cfg.log_level) != 0) {
        fprintf(stderr, "keel: log: %s\n", strerror(errno));
        return 1;
    }

    int root_fd = open(cfg.root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0) {
        keel_log_error("root %s: %s", cfg.root, strerror(errno));
        fprintf(stderr, "keel: root %s: %s\n", cfg.root, strerror(errno));
        keel_log_close();
        return 1;
    }

    struct keel_listener lst;
    if (keel_listen(cfg.bind_addr, cfg.port, &lst) != 0) {
        keel_log_error("listen: %s", strerror(errno));
        fprintf(stderr, "keel: listen: %s\n", strerror(errno));
        close(root_fd);
        keel_log_close();
        return 1;
    }

    fprintf(stderr, "keel %s: listening on %s:%s\n", KEEL_VERSION, lst.local.host, lst.local.port);
    keel_log_info("listening on %s:%s", lst.local.host, lst.local.port);

    struct keel_vhost site;
    fill_vhost(&site, &cfg);
    int rc = keel_serve(lst.fd, root_fd, &cfg.serve, &site);
    close(root_fd);
    keel_listener_close(&lst);
    if (rc != 0) {
        keel_log_error("server stopped: %s", strerror(errno));
        fprintf(stderr, "keel: %s\n", strerror(errno));
        keel_log_close();
        return 1;
    }
    keel_log_close();
    return 0;
}
