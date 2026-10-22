#include "keel/http.h"

#include <string.h>

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int keel_url_decode(const char *src, size_t src_len, char *dst, size_t dst_cap) {
    if (src == NULL || dst == NULL || dst_cap == 0) {
        return -1;
    }
    size_t out = 0;
    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%') {
            if (i + 2 >= src_len) {
                return -1;
            }
            int hi = hex_value((unsigned char)src[i + 1]);
            int lo = hex_value((unsigned char)src[i + 2]);
            if (hi < 0 || lo < 0) {
                return -1;
            }
            c = (unsigned char)((hi << 4) | lo);
            if (c == 0 || c == '/') {
                return -1;
            }
            i += 2;
        }
        if (out + 1 >= dst_cap) {
            return -1;
        }
        dst[out++] = (char)c;
    }
    dst[out] = '\0';
    return 0;
}
