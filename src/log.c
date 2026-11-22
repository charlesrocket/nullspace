#include "log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wayland-client-core.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>

static enum LogLevel current_level = LOG_INFO;
static bool verbose_enabled = false;

static const char* const LEVEL_NAMES[] = {
    [LOG_TRACE] = "trace", [LOG_DEBUG] = "debug", [LOG_INFO] = "info",
    [LOG_WARN] = "warn",   [LOG_ERROR] = "error",
};

static enum wlr_log_importance to_wlr(enum LogLevel lvl) {
    switch (lvl) {
        case LOG_TRACE:
        case LOG_DEBUG: return WLR_DEBUG;
        case LOG_INFO: return WLR_INFO;
        case LOG_WARN:
        case LOG_ERROR: return WLR_ERROR;
    }

    return WLR_INFO;
}

static bool parse_level(const char* s, enum LogLevel* out) {
    for (int i = 0; i < LOG_ERROR + 1; i++) {
        if (strcasecmp(s, LEVEL_NAMES[i]) == 0) {
            *out = (enum LogLevel)i;
            return true;
        }
    }

    if (strcasecmp(s, "warning") == 0) {
        *out = LOG_WARN;
        return true;
    }

    return false;
}

static void wl_log_silent(const char* fmt, va_list args) {
    (void)fmt;
    (void)args;
}

static void apply_wlr_level(void) {
    wlr_log_init(verbose_enabled ? to_wlr(current_level) : WLR_SILENT, NULL);
}

void log_init(void) {
    const char* vrb = getenv("NULLSPACE_VERBOSE");
    verbose_enabled = vrb != NULL && strcmp(vrb, "true") == 0;

    const char* env = getenv("NULLSPACE_LOG_LEVEL");
    if (env && *env && !parse_level(env, &current_level))
        current_level = LOG_INFO;

    if (!verbose_enabled) {
        wl_log_set_handler_server(wl_log_silent);
        wl_log_set_handler_client(wl_log_silent);
    }

    apply_wlr_level();
}

void log_set_level(enum LogLevel level) {
    current_level = level;
    apply_wlr_level();
}

enum LogLevel log_get_level(void) { return current_level; }

void log_msg(
    enum LogLevel level, const char* topic, const char* file, int line,
    const char* fmt, ...
) {
    if (!verbose_enabled) return;
    if (level < current_level) return;

    char prefix[128] = "";
    if (topic && *topic) snprintf(prefix, sizeof prefix, "[%s] ", topic);

    const char* base = NULL;
    if (file) {
        const char* slash = strrchr(file, '/');
        base = slash ? slash + 1 : file;
    }

    va_list args;
    va_start(args, fmt);

    size_t len = strlen(prefix) + strlen(fmt) + 64;
    char* full = malloc(len);
    if (full != NULL) {
        if (base != NULL)
            snprintf(full, len, "%s%s [%s:%d]", prefix, fmt, base, line);
        else
            snprintf(full, len, "%s%s", prefix, fmt);

        _wlr_vlog(to_wlr(level), full, args);
        free(full);
    } else {
        _wlr_vlog(to_wlr(level), fmt, args);
    }

    va_end(args);
}
