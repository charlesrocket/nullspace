#ifndef __BSD_VISIBLE
#define __BSD_VISIBLE 1
#endif

#include "log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static enum LogLevel current_level = LOG_INFO;
static bool use_color = false;

static const char *const LEVEL_NAMES[] = {
    "trace", "debug", "info", "warn", "error",
};

static const char *const level_colors[] = {
    "\x1b[90m", "\x1b[36m", NULL, "\x1b[33m", "\x1b[31m",
};

static enum LogLevel parse_level(const char *s) {
    if (s == NULL || *s == '\0') return LOG_INFO;
    if (strcasecmp(s, "trace") == 0) return LOG_TRACE;
    if (strcasecmp(s, "debug") == 0) return LOG_DEBUG;
    if (strcasecmp(s, "info") == 0) return LOG_INFO;
    if (strcasecmp(s, "warn") == 0) return LOG_WARN;
    if (strcasecmp(s, "warning") == 0) return LOG_WARN;
    if (strcasecmp(s, "error") == 0) return LOG_ERROR;
    return LOG_INFO;
}

void log_init(void) {
    use_color = isatty(fileno(stderr));

    const char *env = getenv("NULLSPACE_LOG_LEVEL");
    if (env != NULL && *env != '\0') current_level = parse_level(env);
}

void log_set_level(enum LogLevel level) {
    if (level < LOG_TRACE) level = LOG_TRACE;
    if (level > LOG_ERROR) level = LOG_ERROR;
    current_level = level;
}

enum LogLevel log_get_level(void) { return current_level; }

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void log_msg(
    enum LogLevel level, const char *topic, const char *file, int line,
    const char *fmt, ...
) {
    if (level < LOG_TRACE) level = LOG_TRACE;
    if (level > LOG_ERROR) level = LOG_ERROR;
    if (level < current_level) return;

    const bool have_topic = topic != NULL && topic[0] != '\0';
    const char *color = use_color ? level_colors[level] : NULL;

    if (color != NULL) {
        fprintf(stderr, "%s%s", color, LEVEL_NAMES[level]);
        if (have_topic) fprintf(stderr, "(%s)", topic);
        fprintf(stderr, ":\x1b[0m ");
    } else {
        fprintf(stderr, "%s", LEVEL_NAMES[level]);
        if (have_topic) fprintf(stderr, "(%s)", topic);
        fprintf(stderr, ": ");
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    if (file != NULL) { fprintf(stderr, " [%s:%d]", basename_of(file), line); }

    fputc('\n', stderr);
}
