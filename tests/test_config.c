#include "test_harness.h"
#include "keel/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_file(const char *path, const char *text) {
    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        return -1;
    }
    fputs(text, fp);
    fclose(fp);
    return 0;
}

int main(void) {
    char tmpl[] = "/tmp/keel-conf-XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL);
    if (dir == NULL) {
        return keel_test_summary();
    }
    char path[128];
    CHECK(strlen(dir) + 12 < sizeof path);
    memcpy(path, dir, strlen(dir));
    memcpy(path + strlen(dir), "/keel.conf", 11);

    CHECK(write_file(path,
                     "# sample\n"
                     "\n"
                     "bind 127.0.0.1\n"
                     "port 9\n"
                     "root \"/var/www\"\n"
                     "workers 4\n"
                     "directory_listing off\n"
                     "index home.html\n"
                     "server_name Example.COM\n"
                     "server_header \"keel test\"\n"
                     "log_level debug\n"
                     "max_header_bytes 1024\n") == 0);

    struct keel_config cfg;
    char arg0[] = "keel";
    char flag[] = "--config";
    char *argv[] = {arg0, flag, path};
    CHECK(keel_config_load(&cfg, 3, argv) == 0);
    CHECK(strcmp(cfg.bind_addr, "127.0.0.1") == 0);
    CHECK(cfg.port == 9);
    CHECK(strcmp(cfg.root, "/var/www") == 0);
    CHECK(cfg.serve.workers == 4);
    CHECK(cfg.directory_listing == false);
    CHECK(strcmp(cfg.index_name, "home.html") == 0);
    CHECK(strcmp(cfg.server_name, "Example.COM") == 0);
    CHECK(strcmp(cfg.server_header, "keel test") == 0);
    CHECK(cfg.log_level == 3);
    CHECK(cfg.serve.max_header_bytes == 1024);

    char portflag[] = "--port";
    char portval[] = "8081";
    char *argv2[] = {arg0, flag, path, portflag, portval};
    CHECK(keel_config_load(&cfg, 5, argv2) == 0);
    CHECK(cfg.port == 8081);
    CHECK(strcmp(cfg.bind_addr, "127.0.0.1") == 0);

    char help[] = "--help";
    char *argvh[] = {arg0, help};
    CHECK(keel_config_load(&cfg, 2, argvh) == 1);

    char ver[] = "--version";
    char *argvv[] = {arg0, ver};
    CHECK(keel_config_load(&cfg, 2, argvv) == 2);

    CHECK(write_file(path, "nope 1\n") == 0);
    CHECK(keel_config_load(&cfg, 3, argv) == -1);

    CHECK(write_file(path, "port 70000\n") == 0);
    CHECK(keel_config_load(&cfg, 3, argv) == -1);

    CHECK(write_file(path, "directory_listing yes\n") == 0);
    CHECK(keel_config_load(&cfg, 3, argv) == 0);
    CHECK(cfg.directory_listing == true);

    unlink(path);
    rmdir(dir);
    return keel_test_summary();
}
