#include "test_harness.h"
#include "keel/http.h"

#include <string.h>

int main(void) {
    CHECK(strcmp(keel_mime_type("index.html"), "text/html; charset=utf-8") == 0);
    CHECK(strcmp(keel_mime_type("dir/PAGE.HTML"), "text/html; charset=utf-8") == 0);
    CHECK(strcmp(keel_mime_type("a/b/c.txt"), "text/plain; charset=utf-8") == 0);
    CHECK(strcmp(keel_mime_type("photo.JPEG"), "image/jpeg") == 0);
    CHECK(strcmp(keel_mime_type("app.js"), "text/javascript; charset=utf-8") == 0);
    CHECK(strcmp(keel_mime_type("data.json"), "application/json") == 0);
    CHECK(strcmp(keel_mime_type("font.woff2"), "font/woff2") == 0);
    CHECK(strcmp(keel_mime_type("pic.svg"), "image/svg+xml") == 0);
    CHECK(strcmp(keel_mime_type("blob"), "application/octet-stream") == 0);
    CHECK(strcmp(keel_mime_type(".gitignore"), "application/octet-stream") == 0);
    CHECK(strcmp(keel_mime_type("archive.tar.gz"), "application/octet-stream") == 0);
    CHECK(strcmp(keel_mime_type("nope.notarealtype"), "application/octet-stream") == 0);
    CHECK(strcmp(keel_mime_type(NULL), "application/octet-stream") == 0);
    return keel_test_summary();
}
