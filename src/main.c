#include "keel/version.h"

#include <stdio.h>

int main(int argc, char **argv) {
    const char *prog = (argc > 0 && argv[0] != NULL) ? argv[0] : "keel";
    printf("keel %s\nusage: %s\n", KEEL_VERSION, prog);
    return 0;
}
