#pragma once

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <nullspace/layouts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

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
