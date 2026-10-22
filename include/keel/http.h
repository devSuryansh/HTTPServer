#ifndef KEEL_HTTP_H
#define KEEL_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define KEEL_METHOD_MAX 32
#define KEEL_TARGET_MAX 2048
#define KEEL_PATH_MAX 2048
#define KEEL_QUERY_MAX 2048
#define KEEL_HOST_HDR_MAX 256
#define KEEL_FIELD_MAX 512
#define KEEL_REQLINE_MAX 512
#define KEEL_RANGE_MAX 128
#define KEEL_ETAG_MAX 256
#define KEEL_IMS_MAX 64
#define KEEL_MAX_HEADERS 64

typedef enum {
    KEEL_PARSE_OK = 0,
    KEEL_PARSE_INCOMPLETE,
    KEEL_PARSE_BAD_REQUEST,
    KEEL_PARSE_URI_TOO_LONG,
    KEEL_PARSE_HEADER_TOO_LARGE,
    KEEL_PARSE_VERSION,
    KEEL_PARSE_NOT_IMPLEMENTED,
    KEEL_PARSE_EXPECTATION,
} keel_parse_status;

typedef enum {
    KEEL_METHOD_GET = 0,
    KEEL_METHOD_HEAD,
    KEEL_METHOD_OPTIONS,
    KEEL_METHOD_POST,
    KEEL_METHOD_PUT,
    KEEL_METHOD_DELETE,
    KEEL_METHOD_PATCH,
    KEEL_METHOD_TRACE,
    KEEL_METHOD_CONNECT,
    KEEL_METHOD_UNKNOWN,
} keel_method;

struct keel_request {
    keel_method method;
    char method_token[KEEL_METHOD_MAX];
    int http_minor;
    bool asterisk;
    char target[KEEL_TARGET_MAX];
    char path[KEEL_PATH_MAX];
    char query[KEEL_QUERY_MAX];
    bool has_query;
    char req_line[KEEL_REQLINE_MAX];
    bool has_host;
    char host[KEEL_HOST_HDR_MAX];
    bool close_connection;
    bool has_content_length;
    uint64_t content_length;
    bool expect;
    bool accept_gzip;
    bool has_range;
    char range[KEEL_RANGE_MAX];
    bool has_if_none_match;
    char if_none_match[KEEL_ETAG_MAX];
    bool has_if_modified_since;
    char if_modified_since[KEEL_IMS_MAX];
    char referer[KEEL_FIELD_MAX];
    char user_agent[KEEL_FIELD_MAX];
    size_t header_count;
};

struct keel_parser {
    size_t offset;
    size_t limit;
    bool saw_request_line;
    bool done;
    bool force_close;
    bool want_keepalive;
    bool has_te;
    keel_parse_status status;
    struct keel_request req;
};

void keel_parser_init(struct keel_parser *parser, size_t limit);

/* buf is the bytes accumulated for this request so far. On OK, *consumed is
   the index just past the header block. */
keel_parse_status keel_parser_execute(struct keel_parser *parser, const char *buf, size_t len,
                                      size_t *consumed);

int keel_parse_to_status(keel_parse_status status);
const char *keel_status_reason(int status);

/* Percent-decode. '+' stays '+'. Rejects bad hex, NUL, and decoded '/'. */
int keel_url_decode(const char *src, size_t src_len, char *dst, size_t dst_cap);

/* Turn a decoded origin-form path into a relative path under the document root.
   rel is empty for the root itself. Returns -1 if the path leaves the root. */
int keel_path_normalize(const char *decoded, char *rel, size_t rel_cap, bool *trailing_slash);

enum keel_open_status {
    KEEL_OPEN_FILE = 0,
    KEEL_OPEN_DIR,
    KEEL_OPEN_NOT_FOUND,
    KEEL_OPEN_FORBIDDEN,
    KEEL_OPEN_ERROR,
};

struct keel_opened {
    int fd;
    bool directory;
    off_t size;
    time_t mtime;
};

enum keel_open_status keel_open_under_root(int root_fd, const char *rel, struct keel_opened *out);
void keel_opened_close(struct keel_opened *out);

struct keel_response {
    int status;
    int http_minor;
    bool head_only;
    bool close_after;
    bool send_allow;
    bool has_location;
    char content_type[128];
    char date[64];
    char location[KEEL_TARGET_MAX];
    int file_fd;
    off_t file_size;
    const char *mem;
    size_t mem_len;
};

void keel_response_init(struct keel_response *resp);
void keel_response_clear(struct keel_response *resp);
void keel_response_set_error(struct keel_response *resp, int status, int http_minor);

const char *keel_mime_type(const char *rel_path);

/* Fill resp for a parsed request. root_fd is a directory descriptor. */
int keel_prepare_static(const struct keel_request *req, int root_fd, const char *index_name,
                        struct keel_response *resp);

/* Write status line, headers, and body. HEAD skips the body. */
int keel_write_blocking(int fd, const struct keel_response *resp);

/* Serialize the status line and headers, including the final blank line. */
int keel_format_head(const struct keel_response *resp, char *dst, size_t cap, size_t *out_len);

/* IMF-fix date in UTC, independent of the process locale. */
int keel_format_http_date(char *dst, size_t cap, time_t when);

#endif
