#include "keel/http.h"

#include <stdlib.h>
#include <string.h>

struct mime_row {
    const char *ext;
    const char *type;
};

static const struct mime_row MIME_TABLE[] = {
    {"avif", "image/avif"},
    {"bmp", "image/bmp"},
    {"css", "text/css; charset=utf-8"},
    {"csv", "text/csv; charset=utf-8"},
    {"gif", "image/gif"},
    {"htm", "text/html; charset=utf-8"},
    {"html", "text/html; charset=utf-8"},
    {"ico", "image/x-icon"},
    {"jpeg", "image/jpeg"},
    {"jpg", "image/jpeg"},
    {"js", "text/javascript; charset=utf-8"},
    {"json", "application/json"},
    {"map", "application/json"},
    {"md", "text/markdown; charset=utf-8"},
    {"mjs", "text/javascript; charset=utf-8"},
    {"mp3", "audio/mpeg"},
    {"mp4", "video/mp4"},
    {"ogg", "audio/ogg"},
    {"otf", "font/otf"},
    {"pdf", "application/pdf"},
    {"png", "image/png"},
    {"svg", "image/svg+xml"},
    {"ttf", "font/ttf"},
    {"txt", "text/plain; charset=utf-8"},
    {"wasm", "application/wasm"},
    {"wav", "audio/wav"},
    {"webm", "video/webm"},
    {"webmanifest", "application/manifest+json"},
    {"webp", "image/webp"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"xhtml", "application/xhtml+xml"},
    {"xml", "text/xml; charset=utf-8"},
};

static int mime_cmp(const void *key, const void *elem) {
    const char *ext = key;
    const struct mime_row *row = elem;
    return strcmp(ext, row->ext);
}

static char ascii_lower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return (char)c;
}

const char *keel_mime_type(const char *rel_path) {
    static const char *fallback = "application/octet-stream";
    if (rel_path == NULL || rel_path[0] == '\0') {
        return fallback;
    }
    const char *base = rel_path;
    for (const char *p = rel_path; *p != '\0'; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    const char *dot = NULL;
    for (const char *p = base; *p != '\0'; p++) {
        if (*p == '.') {
            dot = p;
        }
    }
    if (dot == NULL || dot == base || dot[1] == '\0') {
        return fallback;
    }
    char ext[32];
    size_t n = strlen(dot + 1);
    if (n == 0 || n >= sizeof ext) {
        return fallback;
    }
    for (size_t i = 0; i < n; i++) {
        ext[i] = ascii_lower((unsigned char)dot[1 + i]);
    }
    ext[n] = '\0';
    const struct mime_row *row =
        bsearch(ext, MIME_TABLE, sizeof MIME_TABLE / sizeof MIME_TABLE[0], sizeof MIME_TABLE[0], mime_cmp);
    if (row == NULL) {
        return fallback;
    }
    return row->type;
}
