#pragma once

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <nullspace/layouts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static inline size_t b64_len(size_t n) {
    size_t groups = n / 3 + (n % 3 != 0);
    return groups > SIZE_MAX / 4 ? SIZE_MAX : groups * 4;
}

static inline bool set_nosigpipe(int fd) {
    const int val = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &val, sizeof(val)) == 0;
}

static inline bool ns_parse_int(const char *s, long *out) {
    if (!s || !*s) return false;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return false;
    *out = v;
    return true;
}

static inline bool ns_parse_float(const char *s, float *out) {
    if (!s || !*s) return false;
    char *end = NULL;
    errno = 0;
    float v = strtof(s, &end);
    if (errno != 0 || end == s || *end != '\0' || !isfinite(v)) return false;
    *out = v;
    return true;
}

static inline char *ns_trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

static inline const char *ns_layout_name(enum Layout l) {
    static const struct {
        enum Layout id;
        const char *name;
    } t[] = {
#define NS_LAYOUT_ROW(id, label, desc) {id, label},
        LAYOUTS_TABLE(NS_LAYOUT_ROW)
#undef NS_LAYOUT_ROW
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (t[i].id == l) return t[i].name;
    }
    return "?";
}

static inline bool ns_parse_layout_name(const char *s, enum Layout *out) {
    static const struct {
        enum Layout id;
        const char *name;
    } t[] = {
#define NS_LAYOUT_ROW(id, label, desc) {id, label},
        LAYOUTS_TABLE(NS_LAYOUT_ROW)
#undef NS_LAYOUT_ROW
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (s && !strcmp(s, t[i].name)) {
            *out = t[i].id;
            return true;
        }
    }
    return false;
}

static inline unsigned int ns_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return (unsigned int)(c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (unsigned int)(c - 'A' + 10);
    return 0u;
}

// '#RRGGBB'/ '#RRGGBBAA' straight-alpha
static inline bool ns_parse_hex_color(const char *s, float out[4]) {
    if (s == NULL || out == NULL || s[0] != '#') return false;

    const char *p = s + 1;
    const size_t len = strlen(p);
    if (len != 6 && len != 8) return false;

    for (size_t i = 0; i < len; i++) {
        char c = p[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
               || (c >= 'A' && c <= 'F');

        if (!ok) return false;
    }

    out[0] = (float)(ns_hex_nibble(p[0]) * 16u + ns_hex_nibble(p[1])) / 255.0f;
    out[1] = (float)(ns_hex_nibble(p[2]) * 16u + ns_hex_nibble(p[3])) / 255.0f;
    out[2] = (float)(ns_hex_nibble(p[4]) * 16u + ns_hex_nibble(p[5])) / 255.0f;
    out[3] =
        (len == 8)
            ? (float)(ns_hex_nibble(p[6]) * 16u + ns_hex_nibble(p[7])) / 255.0f
            : 1.0f;

    return true;
}
