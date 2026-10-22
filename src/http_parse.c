#include "keel/http.h"

#include <stdio.h>
#include <string.h>

static unsigned char ascii_lower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return (unsigned char)(c + ('a' - 'A'));
    }
    return c;
}

static bool bytes_eq_i(const char *s, size_t n, const char *lit) {
    size_t m = strlen(lit);
    if (n != m) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (ascii_lower((unsigned char)s[i]) != ascii_lower((unsigned char)lit[i])) {
            return false;
        }
    }
    return true;
}

static bool is_tchar(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

static bool copy_fit(char *dst, size_t cap, const char *src, size_t n) {
    if (n >= cap) {
        return false;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return true;
}

static void copy_trunc(char *dst, size_t cap, const char *src, size_t n) {
    if (cap == 0) {
        return;
    }
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int parse_u64(const char *s, size_t n, uint64_t *out) {
    if (n == 0) {
        return -1;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        uint64_t d = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10ull) {
            return -1;
        }
        v = v * 10ull + d;
    }
    *out = v;
    return 0;
}

static keel_method method_of(const char *s, size_t n) {
    static const struct {
        const char *name;
        size_t len;
        keel_method method;
    } table[] = {
        {"GET", 3, KEEL_METHOD_GET},       {"HEAD", 4, KEEL_METHOD_HEAD},
        {"OPTIONS", 7, KEEL_METHOD_OPTIONS}, {"POST", 4, KEEL_METHOD_POST},
        {"PUT", 3, KEEL_METHOD_PUT},       {"DELETE", 6, KEEL_METHOD_DELETE},
        {"PATCH", 5, KEEL_METHOD_PATCH},   {"TRACE", 5, KEEL_METHOD_TRACE},
        {"CONNECT", 7, KEEL_METHOD_CONNECT},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (n == table[i].len && memcmp(s, table[i].name, n) == 0) {
            return table[i].method;
        }
    }
    return KEEL_METHOD_UNKNOWN;
}

static keel_parse_status parse_version(const char *s, size_t n, int *minor) {
    if (n < 7 || memcmp(s, "HTTP/", 5) != 0) {
        return KEEL_PARSE_BAD_REQUEST;
    }
    size_t i = 5;
    if (s[i] < '0' || s[i] > '9') {
        return KEEL_PARSE_BAD_REQUEST;
    }
    int major = 0;
    int major_digits = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        major = major * 10 + (s[i] - '0');
        major_digits++;
        if (major_digits > 3) {
            return KEEL_PARSE_BAD_REQUEST;
        }
        i++;
    }
    if (i >= n || s[i] != '.') {
        return KEEL_PARSE_BAD_REQUEST;
    }
    i++;
    if (i >= n || s[i] < '0' || s[i] > '9') {
        return KEEL_PARSE_BAD_REQUEST;
    }
    int minv = 0;
    int minor_digits = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        minv = minv * 10 + (s[i] - '0');
        minor_digits++;
        if (minor_digits > 3) {
            return KEEL_PARSE_BAD_REQUEST;
        }
        i++;
    }
    if (i != n) {
        return KEEL_PARSE_BAD_REQUEST;
    }
    if (major == 1 && (minv == 0 || minv == 1)) {
        *minor = minv;
        return KEEL_PARSE_OK;
    }
    return KEEL_PARSE_VERSION;
}

static bool param_q_is_zero(const char *s, size_t n) {
    for (size_t i = 0; i + 1 < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c == 'q' || c == 'Q') && s[i + 1] == '=') {
            bool boundary = (i == 0) || s[i - 1] == ';' || s[i - 1] == ' ' || s[i - 1] == '\t';
            if (!boundary) {
                continue;
            }
            size_t j = i + 2;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) {
                j++;
            }
            if (j >= n || s[j] != '0') {
                return false;
            }
            j++;
            if (j < n && s[j] == '.') {
                j++;
                while (j < n && s[j] == '0') {
                    j++;
                }
                if (j < n && s[j] >= '1' && s[j] <= '9') {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

static bool accepts_gzip(const char *v, size_t n) {
    size_t i = 0;
    bool yes = false;
    while (i < n) {
        while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) {
            i++;
        }
        if (i >= n) {
            break;
        }
        size_t start = i;
        while (i < n && v[i] != ',') {
            i++;
        }
        size_t end = i;
        while (start < end && (v[start] == ' ' || v[start] == '\t')) {
            start++;
        }
        while (end > start && (v[end - 1] == ' ' || v[end - 1] == '\t')) {
            end--;
        }
        size_t semi = start;
        while (semi < end && v[semi] != ';') {
            semi++;
        }
        size_t coding_end = semi;
        while (coding_end > start && (v[coding_end - 1] == ' ' || v[coding_end - 1] == '\t')) {
            coding_end--;
        }
        if (coding_end - start == 4 && bytes_eq_i(v + start, 4, "gzip")) {
            if (!param_q_is_zero(v + semi, end - semi)) {
                yes = true;
            }
        }
    }
    return yes;
}

static void apply_connection(struct keel_parser *p, const char *v, size_t n) {
    size_t i = 0;
    while (i < n) {
        while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) {
            i++;
        }
        if (i >= n) {
            break;
        }
        size_t start = i;
        while (i < n && v[i] != ',') {
            i++;
        }
        size_t end = i;
        while (start < end && (v[start] == ' ' || v[start] == '\t')) {
            start++;
        }
        while (end > start && (v[end - 1] == ' ' || v[end - 1] == '\t')) {
            end--;
        }
        size_t len = end - start;
        if (bytes_eq_i(v + start, len, "close")) {
            p->force_close = true;
        } else if (bytes_eq_i(v + start, len, "keep-alive")) {
            p->want_keepalive = true;
        }
    }
}

static keel_parse_status parse_request_line(struct keel_parser *p, const char *line, size_t len) {
    copy_trunc(p->req.req_line, sizeof p->req.req_line, line, len);

    size_t i = 0;
    while (i < len && line[i] != ' ') {
        if (!is_tchar((unsigned char)line[i])) {
            return KEEL_PARSE_BAD_REQUEST;
        }
        i++;
    }
    size_t method_len = i;
    if (method_len == 0 || i >= len || line[i] != ' ') {
        return KEEL_PARSE_BAD_REQUEST;
    }
    if (method_len >= KEEL_METHOD_MAX) {
        return KEEL_PARSE_BAD_REQUEST;
    }
    i++;
    if (i < len && line[i] == ' ') {
        return KEEL_PARSE_BAD_REQUEST;
    }

    size_t target_at = i;
    while (i < len && line[i] != ' ') {
        i++;
    }
    size_t target_len = i - target_at;
    if (target_len == 0 || i >= len || line[i] != ' ') {
        return KEEL_PARSE_BAD_REQUEST;
    }
    i++;
    if (i < len && line[i] == ' ') {
        return KEEL_PARSE_BAD_REQUEST;
    }
    size_t version_at = i;
    size_t version_len = len - version_at;
    if (version_len == 0) {
        return KEEL_PARSE_BAD_REQUEST;
    }

    if (!copy_fit(p->req.method_token, sizeof p->req.method_token, line, method_len)) {
        return KEEL_PARSE_BAD_REQUEST;
    }
    p->req.method = method_of(line, method_len);

    int minor = 1;
    keel_parse_status vst = parse_version(line + version_at, version_len, &minor);
    if (vst != KEEL_PARSE_OK) {
        return vst;
    }
    p->req.http_minor = minor;
    p->req.close_connection = (minor == 0);

    if (target_len >= KEEL_TARGET_MAX) {
        return KEEL_PARSE_URI_TOO_LONG;
    }
    if (!copy_fit(p->req.target, sizeof p->req.target, line + target_at, target_len)) {
        return KEEL_PARSE_URI_TOO_LONG;
    }

    const char *target = line + target_at;
    if (target_len == 1 && target[0] == '*') {
        p->req.asterisk = true;
        memcpy(p->req.path, "*", 2);
        return KEEL_PARSE_OK;
    }
    if (target[0] != '/') {
        return KEEL_PARSE_BAD_REQUEST;
    }

    size_t path_len = target_len;
    size_t qmark = 0;
    while (qmark < target_len && target[qmark] != '?') {
        qmark++;
    }
    path_len = qmark;
    if (qmark < target_len) {
        size_t query_len = target_len - qmark - 1;
        if (query_len >= KEEL_QUERY_MAX) {
            return KEEL_PARSE_URI_TOO_LONG;
        }
        if (!copy_fit(p->req.query, sizeof p->req.query, target + qmark + 1, query_len)) {
            return KEEL_PARSE_URI_TOO_LONG;
        }
        p->req.has_query = true;
    }
    if (path_len >= KEEL_PATH_MAX) {
        return KEEL_PARSE_URI_TOO_LONG;
    }
    if (keel_url_decode(target, path_len, p->req.path, sizeof p->req.path) != 0) {
        return KEEL_PARSE_BAD_REQUEST;
    }
    return KEEL_PARSE_OK;
}

static keel_parse_status parse_header(struct keel_parser *p, const char *line, size_t len) {
    if (p->req.header_count >= KEEL_MAX_HEADERS) {
        return KEEL_PARSE_HEADER_TOO_LARGE;
    }
    size_t colon = 0;
    while (colon < len && line[colon] != ':') {
        if (!is_tchar((unsigned char)line[colon])) {
            return KEEL_PARSE_BAD_REQUEST;
        }
        colon++;
    }
    if (colon == 0 || colon >= len) {
        return KEEL_PARSE_BAD_REQUEST;
    }
    size_t val = colon + 1;
    while (val < len && (line[val] == ' ' || line[val] == '\t')) {
        val++;
    }
    size_t end = len;
    while (end > val && (line[end - 1] == ' ' || line[end - 1] == '\t')) {
        end--;
    }
    for (size_t i = val; i < end; i++) {
        unsigned char c = (unsigned char)line[i];
        if ((c < 32 && c != '\t') || c == 127) {
            return KEEL_PARSE_BAD_REQUEST;
        }
    }
    const char *name = line;
    const char *value = line + val;
    size_t value_len = end - val;
    p->req.header_count++;

    if (bytes_eq_i(name, colon, "Host")) {
        if (p->req.has_host || value_len == 0) {
            return KEEL_PARSE_BAD_REQUEST;
        }
        if (!copy_fit(p->req.host, sizeof p->req.host, value, value_len)) {
            return KEEL_PARSE_BAD_REQUEST;
        }
        p->req.has_host = true;
        return KEEL_PARSE_OK;
    }
    if (bytes_eq_i(name, colon, "Connection")) {
        apply_connection(p, value, value_len);
        return KEEL_PARSE_OK;
    }
    if (bytes_eq_i(name, colon, "Content-Length")) {
        uint64_t n = 0;
        if (parse_u64(value, value_len, &n) != 0) {
            return KEEL_PARSE_BAD_REQUEST;
        }
        if (p->req.has_content_length && p->req.content_length != n) {
            return KEEL_PARSE_BAD_REQUEST;
        }
        p->req.has_content_length = true;
        p->req.content_length = n;
        return KEEL_PARSE_OK;
    }
    if (bytes_eq_i(name, colon, "Transfer-Encoding")) {
        p->has_te = true;
        return KEEL_PARSE_OK;
    }
    if (bytes_eq_i(name, colon, "Expect")) {
        p->req.expect = true;
        return KEEL_PARSE_OK;
    }
    if (bytes_eq_i(name, colon, "Accept-Encoding")) {
        if (accepts_gzip(value, value_len)) {
            p->req.accept_gzip = true;
        }
        return KEEL_PARSE_OK;
    }
    if (bytes_eq_i(name, colon, "Range")) {
        if (copy_fit(p->req.range, sizeof p->req.range, value, value_len)) {
            p->req.has_range = true;
        } else {
            p->req.has_range = false;
        }
        return KEEL_PARSE_OK;
    }
    if (bytes_eq_i(name, colon, "If-None-Match")) {
        if (copy_fit(p->req.if_none_match, sizeof p->req.if_none_match, value, value_len)) {
            p->req.has_if_none_match = true;
        } else {
            p->req.has_if_none_match = false;
        }
        return KEEL_PARSE_OK;
    }
    if (bytes_eq_i(name, colon, "If-Modified-Since")) {
        if (copy_fit(p->req.if_modified_since, sizeof p->req.if_modified_since, value, value_len)) {
            p->req.has_if_modified_since = true;
        } else {
            p->req.has_if_modified_since = false;
        }
        return KEEL_PARSE_OK;
    }
    if (bytes_eq_i(name, colon, "Referer")) {
        copy_trunc(p->req.referer, sizeof p->req.referer, value, value_len);
        return KEEL_PARSE_OK;
    }
    if (bytes_eq_i(name, colon, "User-Agent")) {
        copy_trunc(p->req.user_agent, sizeof p->req.user_agent, value, value_len);
        return KEEL_PARSE_OK;
    }
    return KEEL_PARSE_OK;
}

static keel_parse_status finalize_request(struct keel_parser *p) {
    if (p->force_close) {
        p->req.close_connection = true;
    } else if (p->want_keepalive) {
        p->req.close_connection = false;
    }
    if (p->has_te && p->req.has_content_length) {
        return KEEL_PARSE_BAD_REQUEST;
    }
    if (p->has_te || (p->req.has_content_length && p->req.content_length > 0)) {
        return KEEL_PARSE_NOT_IMPLEMENTED;
    }
    if (p->req.expect) {
        return KEEL_PARSE_EXPECTATION;
    }
    return KEEL_PARSE_OK;
}

static keel_parse_status classify_overflow(const struct keel_parser *p, const char *buf, size_t n) {
    if (!p->saw_request_line) {
        size_t i = 0;
        while (i < n && buf[i] != ' ' && buf[i] != '\0' && buf[i] != '\r' && buf[i] != '\n') {
            i++;
        }
        if (i < n && buf[i] == ' ') {
            i++;
            size_t start = i;
            while (i < n && buf[i] != ' ' && buf[i] != '\0' && buf[i] != '\r' && buf[i] != '\n') {
                i++;
            }
            if (i - start >= (size_t)KEEL_TARGET_MAX) {
                return KEEL_PARSE_URI_TOO_LONG;
            }
        }
    }
    return KEEL_PARSE_HEADER_TOO_LARGE;
}

static keel_parse_status finish(struct keel_parser *p, keel_parse_status st, size_t *consumed) {
    p->done = true;
    p->status = st;
    if (consumed != NULL) {
        *consumed = p->offset;
    }
    return st;
}

/* 0: keep scanning, 1: headers finished, -1: error. Status is written to *st. */
static int take_line(struct keel_parser *p, const char *line, size_t len, keel_parse_status *st) {
    if (len == 0) {
        if (!p->saw_request_line) {
            *st = KEEL_PARSE_BAD_REQUEST;
            return -1;
        }
        *st = finalize_request(p);
        return 1;
    }
    if (!p->saw_request_line) {
        p->saw_request_line = true;
        *st = parse_request_line(p, line, len);
        return *st == KEEL_PARSE_OK ? 0 : -1;
    }
    if (line[0] == ' ' || line[0] == '\t') {
        *st = KEEL_PARSE_BAD_REQUEST;
        return -1;
    }
    *st = parse_header(p, line, len);
    return *st == KEEL_PARSE_OK ? 0 : -1;
}

void keel_parser_init(struct keel_parser *parser, size_t limit) {
    memset(parser, 0, sizeof *parser);
    parser->limit = limit;
    parser->status = KEEL_PARSE_INCOMPLETE;
    parser->req.http_minor = 1;
    parser->req.method = KEEL_METHOD_UNKNOWN;
}

keel_parse_status keel_parser_execute(struct keel_parser *parser, const char *buf, size_t len,
                                      size_t *consumed) {
    if (parser->done) {
        if (consumed != NULL) {
            *consumed = parser->offset;
        }
        return parser->status;
    }
    if (buf == NULL) {
        return finish(parser, KEEL_PARSE_BAD_REQUEST, consumed);
    }

    while (parser->offset < parser->limit && parser->offset < len) {
        size_t begin = parser->offset;
        size_t window = parser->limit < len ? parser->limit : len;
        size_t j = begin;
        while (j < window) {
            unsigned char c = (unsigned char)buf[j];
            if (c == 0) {
                return finish(parser, KEEL_PARSE_BAD_REQUEST, consumed);
            }
            if (c == '\n') {
                break;
            }
            j++;
        }
        if (j == window) {
            if (len >= parser->limit) {
                return finish(parser, classify_overflow(parser, buf, window), consumed);
            }
            if (consumed != NULL) {
                *consumed = parser->offset;
            }
            return KEEL_PARSE_INCOMPLETE;
        }
        if (j == begin || buf[j - 1] != '\r') {
            return finish(parser, KEEL_PARSE_BAD_REQUEST, consumed);
        }
        size_t line_len = j - begin - 1;
        parser->offset = j + 1;
        keel_parse_status st = KEEL_PARSE_OK;
        int kind = take_line(parser, buf + begin, line_len, &st);
        if (kind != 0) {
            return finish(parser, st, consumed);
        }
    }

    if (len >= parser->limit) {
        return finish(parser, classify_overflow(parser, buf, parser->limit), consumed);
    }
    if (consumed != NULL) {
        *consumed = parser->offset;
    }
    return KEEL_PARSE_INCOMPLETE;
}

int keel_parse_to_status(keel_parse_status status) {
    switch (status) {
    case KEEL_PARSE_BAD_REQUEST:
        return 400;
    case KEEL_PARSE_URI_TOO_LONG:
        return 414;
    case KEEL_PARSE_HEADER_TOO_LARGE:
        return 431;
    case KEEL_PARSE_VERSION:
        return 505;
    case KEEL_PARSE_NOT_IMPLEMENTED:
        return 501;
    case KEEL_PARSE_EXPECTATION:
        return 417;
    case KEEL_PARSE_OK:
    case KEEL_PARSE_INCOMPLETE:
        return 0;
    }
    return 400;
}

const char *keel_status_reason(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 206:
        return "Partial Content";
    case 301:
        return "Moved Permanently";
    case 304:
        return "Not Modified";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 408:
        return "Request Timeout";
    case 414:
        return "URI Too Long";
    case 416:
        return "Range Not Satisfiable";
    case 417:
        return "Expectation Failed";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "Error";
    }
}
