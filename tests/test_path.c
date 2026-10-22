#include "test_harness.h"
#include "keel/http.h"

#include <string.h>

static void expect_rel(const char *path, const char *want, bool slash) {
    char rel[KEEL_PATH_MAX];
    bool got_slash = false;
    CHECK(keel_path_normalize(path, rel, sizeof rel, &got_slash) == 0);
    CHECK(strcmp(rel, want) == 0);
    CHECK(got_slash == slash);
}

int main(void) {
    expect_rel("/", "", true);
    expect_rel("/a/b", "a/b", false);
    expect_rel("/a/b/", "a/b", true);
    expect_rel("/a/../b", "b", false);
    expect_rel("/a/./b", "a/b", false);
    expect_rel("/a//b", "a/b", false);
    expect_rel("/foo/..", "", true);
    expect_rel("/sub/../hello.txt", "hello.txt", false);
    expect_rel("/hello world.txt", "hello world.txt", false);

    char rel[KEEL_PATH_MAX];
    bool slash = false;
    CHECK(keel_path_normalize("/../x", rel, sizeof rel, &slash) != 0);
    CHECK(keel_path_normalize("/a/../../b", rel, sizeof rel, &slash) != 0);
    CHECK(keel_path_normalize("/..", rel, sizeof rel, &slash) != 0);
    CHECK(keel_path_normalize("relative", rel, sizeof rel, &slash) != 0);
    CHECK(keel_path_normalize("/a\\b", rel, sizeof rel, &slash) != 0);
    CHECK(keel_path_normalize(NULL, rel, sizeof rel, &slash) != 0);

    return keel_test_summary();
}
