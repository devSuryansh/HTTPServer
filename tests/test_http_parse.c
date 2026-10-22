#include "test_harness.h"
#include "keel/http.h"

#include <stdio.h>
#include <string.h>

static keel_parse_status parse_all(const char *s, size_t limit, struct keel_request *out,
                                   size_t *consumed) {
    struct keel_parser parser;
    keel_parser_init(&parser, limit);
    size_t n = strlen(s);
    size_t got = 0;
    keel_parse_status st = keel_parser_execute(&parser, s, n, &got);
    if (out != NULL) {
        *out = parser.req;
    }
    if (consumed != NULL) {
        *consumed = got;
    }
    return st;
}

static void test_simple_get(void) {
    struct keel_request req;
    size_t consumed = 0;
    const char *msg = "GET /hello.txt HTTP/1.1\r\nHost: example.com\r\n\r\nTRAIL";
    keel_parse_status st = parse_all(msg, 8192, &req, &consumed);
    CHECK(st == KEEL_PARSE_OK);
    CHECK(req.method == KEEL_METHOD_GET);
    CHECK(strcmp(req.method_token, "GET") == 0);
    CHECK(req.http_minor == 1);
    CHECK(strcmp(req.path, "/hello.txt") == 0);
    CHECK(req.has_query == false);
    CHECK(req.has_host == true);
    CHECK(strcmp(req.host, "example.com") == 0);
    CHECK(req.close_connection == false);
    CHECK(strcmp(msg + consumed, "TRAIL") == 0);
}

static void test_http10_defaults_to_close(void) {
    struct keel_request req;
    const char *msg = "GET / HTTP/1.0\r\n\r\n";
    CHECK(parse_all(msg, 8192, &req, NULL) == KEEL_PARSE_OK);
    CHECK(req.http_minor == 0);
    CHECK(req.has_host == false);
    CHECK(req.close_connection == true);
}

static void test_connection_tokens(void) {
    struct keel_request req;
    CHECK(parse_all("GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n", 8192, &req, NULL) ==
          KEEL_PARSE_OK);
    CHECK(req.close_connection == true);

    CHECK(parse_all("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", 8192, &req, NULL) ==
          KEEL_PARSE_OK);
    CHECK(req.close_connection == false);

    CHECK(parse_all("GET / HTTP/1.1\r\nHost: a\r\nConnection: keep-alive, close\r\n\r\n", 8192,
                    &req, NULL) == KEEL_PARSE_OK);
    CHECK(req.close_connection == true);
}

static void test_byte_at_a_time(void) {
    const char *msg = "GET /a%20b+c?x=1 HTTP/1.1\r\n"
                      "hOsT: Example.COM\r\n"
                      "User-Agent: keel-test\r\n"
                      "Accept-Encoding: gzip, deflate\r\n"
                      "\r\n";
    char buf[512];
    size_t n = 0;
    struct keel_parser parser;
    keel_parser_init(&parser, sizeof buf);
    size_t full = strlen(msg);
    for (size_t i = 0; i < full; i++) {
        buf[n++] = msg[i];
        size_t consumed = 0;
        keel_parse_status st = keel_parser_execute(&parser, buf, n, &consumed);
        if (i + 1 < full) {
            CHECK(st == KEEL_PARSE_INCOMPLETE);
        } else {
            CHECK(st == KEEL_PARSE_OK);
            CHECK(consumed == full);
        }
    }
    CHECK(parser.req.method == KEEL_METHOD_GET);
    CHECK(strcmp(parser.req.path, "/a b+c") == 0);
    CHECK(parser.req.has_query == true);
    CHECK(strcmp(parser.req.query, "x=1") == 0);
    CHECK(strcmp(parser.req.host, "Example.COM") == 0);
    CHECK(strcmp(parser.req.user_agent, "keel-test") == 0);
    CHECK(parser.req.accept_gzip == true);
}

static void test_split_crlf(void) {
    const char *msg = "HEAD /idx HTTP/1.1\r\nHost: h\r\n\r\n";
    struct keel_parser parser;
    keel_parser_init(&parser, 8192);
    size_t cut = strlen(msg) - 1;
    size_t consumed = 0;
    CHECK(keel_parser_execute(&parser, msg, cut, &consumed) == KEEL_PARSE_INCOMPLETE);
    CHECK(keel_parser_execute(&parser, msg, strlen(msg), &consumed) == KEEL_PARSE_OK);
    CHECK(parser.req.method == KEEL_METHOD_HEAD);
    CHECK(strcmp(parser.req.path, "/idx") == 0);
}

static void test_decoding_and_query(void) {
    struct keel_request req;
    CHECK(parse_all("GET /%2e%2e/%2E HTTP/1.1\r\nHost: a\r\n\r\n", 8192, &req, NULL) ==
          KEEL_PARSE_OK);
    CHECK(strcmp(req.path, "/../.") == 0);

    CHECK(parse_all("GET /a%2Fb HTTP/1.1\r\nHost: a\r\n\r\n", 8192, &req, NULL) ==
          KEEL_PARSE_BAD_REQUEST);
    CHECK(parse_all("GET /a%00b HTTP/1.1\r\nHost: a\r\n\r\n", 8192, &req, NULL) ==
          KEEL_PARSE_BAD_REQUEST);
    CHECK(parse_all("GET /a%2 HTTP/1.1\r\nHost: a\r\n\r\n", 8192, &req, NULL) ==
          KEEL_PARSE_BAD_REQUEST);
    CHECK(parse_all("GET /a%zz HTTP/1.1\r\nHost: a\r\n\r\n", 8192, &req, NULL) ==
          KEEL_PARSE_BAD_REQUEST);
    CHECK(parse_all("GET /only? HTTP/1.1\r\nHost: a\r\n\r\n", 8192, &req, NULL) == KEEL_PARSE_OK);
    CHECK(strcmp(req.path, "/only") == 0);
    CHECK(req.has_query == true);
    CHECK(req.query[0] == '\0');
}

static void test_url_decode_helper(void) {
    char dst[32];
    CHECK(keel_url_decode("a+b%20c", 7, dst, sizeof dst) == 0);
    CHECK(strcmp(dst, "a+b c") == 0);
    CHECK(keel_url_decode("%2f", 3, dst, sizeof dst) != 0);
    CHECK(keel_url_decode("%00", 3, dst, sizeof dst) != 0);
    CHECK(keel_url_decode("%", 1, dst, sizeof dst) != 0);
    CHECK(keel_url_decode("ok", 2, dst, 2) != 0);
}

static void test_bad_request_line(void) {
    struct keel_request req;
    CHECK(parse_all("GET / HTTP/1.1\nHost: a\r\n\r\n", 8192, &req, NULL) == KEEL_PARSE_BAD_REQUEST);
    CHECK(parse_all("GET  / HTTP/1.1\r\nHost: a\r\n\r\n", 8192, &req, NULL) ==
          KEEL_PARSE_BAD_REQUEST);
    CHECK(parse_all("GET / HTTP/1.1 \r\nHost: a\r\n\r\n", 8192, &req, NULL) ==
          KEEL_PARSE_BAD_REQUEST);
    CHECK(parse_all("GET /\tHTTP/1.1\r\nHost: a\r\n\r\n", 8192, &req, NULL) ==
          KEEL_PARSE_BAD_REQUEST);
    CHECK(parse_all("\r\n\r\n", 8192, &req, NULL) == KEEL_PARSE_BAD_REQUEST);
    CHECK(parse_all("GET http://example/ HTTP/1.1\r\nHost: a\r\n\r\n", 8192, &req, NULL) ==
          KEEL_PARSE_BAD_REQUEST);
    CHECK(parse_all("GET / HTTP/1\r\nHost: a\r\n\r\n", 8192, &req, NULL) == KEEL_PARSE_BAD_REQUEST);
    CHECK(parse_all("GET / HTTP/2.0\r\nHost: a\r\n\r\n", 8192, &req, NULL) == KEEL_PARSE_VERSION);
    CHECK(parse_all("GET / HTTP/1.2\r\nHost: a\r\n\r\n", 8192, &req, NULL) == KEEL_PARSE_VERSION);
    CHECK(parse_all("GOT / HTTP/1.1\r\nHost: a\r\n\r\n", 8192, &req, NULL) == KEEL_PARSE_OK);
    CHECK(req.method == KEEL_METHOD_UNKNOWN);
    CHECK(strcmp(req.method_token, "GOT") == 0);
}

static void test_asterisk(void) {
    struct keel_request req;
    CHECK(parse_all("OPTIONS * HTTP/1.1\r\nHost: a\r\n\r\n", 8192, &req, NULL) == KEEL_PARSE_OK);
    CHECK(req.method == KEEL_METHOD_OPTIONS);
    CHECK(req.asterisk == true);
    CHECK(strcmp(req.path, "*") == 0);
}

static void test_obs_fold_and_duplicate_host(void) {
    struct keel_request req;
    CHECK(parse_all("GET / HTTP/1.1\r\nHost: a\r\n X: b\r\n\r\n", 8192, &req, NULL) ==
          KEEL_PARSE_BAD_REQUEST);
    CHECK(parse_all("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 8192, &req, NULL) ==
          KEEL_PARSE_BAD_REQUEST);

    char raw[64];
    const char *pre = "GET / HTTP/1.1\r\nHost: a\r\nX-Nul: b";
    size_t n = strlen(pre);
    memcpy(raw, pre, n);
    raw[n++] = '\0';
    memcpy(raw + n, "c\r\n\r\n", 5);
    n += 5;
    struct keel_parser parser;
    keel_parser_init(&parser, sizeof raw);
    size_t consumed = 0;
    CHECK(keel_parser_execute(&parser, raw, n, &consumed) == KEEL_PARSE_BAD_REQUEST);
}

static void test_lengths_and_expect(void) {
    struct keel_request req;
    CHECK(parse_all("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n", 8192, &req, NULL) ==
          KEEL_PARSE_OK);
    CHECK(req.has_content_length == true);
    CHECK(req.content_length == 0);

    CHECK(parse_all("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n",
                    8192, &req, NULL) == KEEL_PARSE_OK);

    CHECK(parse_all("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\nContent-Length: 1\r\n\r\n",
                    8192, &req, NULL) == KEEL_PARSE_BAD_REQUEST);

    CHECK(parse_all("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\n\r\n", 8192, &req, NULL) ==
          KEEL_PARSE_NOT_IMPLEMENTED);
    CHECK(req.method == KEEL_METHOD_POST);

    CHECK(parse_all("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", 8192, &req,
                    NULL) == KEEL_PARSE_NOT_IMPLEMENTED);

    CHECK(parse_all("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n",
                    8192, &req, NULL) == KEEL_PARSE_BAD_REQUEST);

    CHECK(parse_all("GET / HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\n\r\n", 8192, &req,
                    NULL) == KEEL_PARSE_EXPECTATION);

    CHECK(parse_all("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\n",
                    8192, &req, NULL) == KEEL_PARSE_NOT_IMPLEMENTED);
}

static void test_limits(void) {
    struct keel_request req;
    char big[4200];
    size_t n = 0;
    memcpy(big, "GET /", 5);
    n = 5;
    while (n < 5 + 3000) {
        big[n++] = 'a';
    }
    const char *tail = " HTTP/1.1\r\nHost: a\r\n\r\n";
    memcpy(big + n, tail, strlen(tail) + 1);
    CHECK(parse_all(big, sizeof big, &req, NULL) == KEEL_PARSE_URI_TOO_LONG);

    const char *wide = "GET / HTTP/1.1\r\nHost: abcdefghijklmnopqrstuvwxyz\r\n\r\n";
    CHECK(parse_all(wide, 32, &req, NULL) == KEEL_PARSE_HEADER_TOO_LARGE);

    char many[4096];
    size_t m = 0;
    const char *prefix = "GET / HTTP/1.1\r\nHost: a\r\n";
    memcpy(many, prefix, strlen(prefix));
    m = strlen(prefix);
    for (int i = 0; i < 70; i++) {
        int wrote = snprintf(many + m, sizeof many - m, "X-%d: v\r\n", i);
        CHECK(wrote > 0);
        m += (size_t)wrote;
    }
    memcpy(many + m, "\r\n", 3);
    CHECK(parse_all(many, sizeof many, &req, NULL) == KEEL_PARSE_HEADER_TOO_LARGE);
}

static void test_selected_headers(void) {
    struct keel_request req;
    const char *msg = "GET /file HTTP/1.1\r\n"
                      "Host: a\r\n"
                      "Range: bytes=0-3\r\n"
                      "If-None-Match: \"abc\"\r\n"
                      "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
                      "Referer: http://a/b\r\n"
                      "Accept-Encoding: identity;q=1, gzip;q=0\r\n"
                      "\r\n";
    CHECK(parse_all(msg, 8192, &req, NULL) == KEEL_PARSE_OK);
    CHECK(req.has_range == true);
    CHECK(strcmp(req.range, "bytes=0-3") == 0);
    CHECK(req.has_if_none_match == true);
    CHECK(strcmp(req.if_none_match, "\"abc\"") == 0);
    CHECK(req.has_if_modified_since == true);
    CHECK(strcmp(req.referer, "http://a/b") == 0);
    CHECK(req.accept_gzip == false);

    CHECK(parse_all("GET / HTTP/1.1\r\nHost: a\r\nAccept-Encoding: br;q=1.0, gzip;q=0.8\r\n\r\n",
                    8192, &req, NULL) == KEEL_PARSE_OK);
    CHECK(req.accept_gzip == true);
}

static void test_status_mapping(void) {
    CHECK(keel_parse_to_status(KEEL_PARSE_OK) == 0);
    CHECK(keel_parse_to_status(KEEL_PARSE_BAD_REQUEST) == 400);
    CHECK(keel_parse_to_status(KEEL_PARSE_URI_TOO_LONG) == 414);
    CHECK(keel_parse_to_status(KEEL_PARSE_HEADER_TOO_LARGE) == 431);
    CHECK(keel_parse_to_status(KEEL_PARSE_VERSION) == 505);
    CHECK(keel_parse_to_status(KEEL_PARSE_NOT_IMPLEMENTED) == 501);
    CHECK(keel_parse_to_status(KEEL_PARSE_EXPECTATION) == 417);
    CHECK(strcmp(keel_status_reason(404), "Not Found") == 0);
    CHECK(strcmp(keel_status_reason(200), "OK") == 0);
}

int main(void) {
    test_simple_get();
    test_http10_defaults_to_close();
    test_connection_tokens();
    test_byte_at_a_time();
    test_split_crlf();
    test_decoding_and_query();
    test_url_decode_helper();
    test_bad_request_line();
    test_asterisk();
    test_obs_fold_and_duplicate_host();
    test_lengths_and_expect();
    test_limits();
    test_selected_headers();
    test_status_mapping();
    return keel_test_summary();
}
