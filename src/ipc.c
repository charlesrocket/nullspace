#include "ipc.h"

#include "include/layouts.h"
#include "input.h"
#include "keys.h"
#include "layouts/layout.h"
#include "log.h"
#include "manager.h"
#include "output.h"
#include "server.h"
#include "view.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-util.h>

#define LOG_TOPIC       "ipc"
#define IPC_INBUF_SIZE  4096
#define IPC_OUTBUF_SOFT 4096
#define IPC_OUTBUF_HARD (256 * 1024)

struct IpcClient {
    struct wl_list link;
    struct wl_event_source *source;

    char inbuf[IPC_INBUF_SIZE];
    char *outbuf;

    int fd;

    bool subscribed;
    bool dead;
    bool read_closed;
    bool write_armed;

    size_t inlen;
    size_t outlen;
    size_t outcap;
};

enum IpcCommandKind {
    IPC_CMD_SWITCH_SPACE,
    IPC_CMD_MOVE_TO_SPACE,
    IPC_CMD_SET_LAYOUT,
    IPC_CMD_CYCLE_LAYOUT,
    IPC_CMD_CLOSE,
};

struct IpcCommand {
    struct wl_list link;
    enum IpcCommandKind kind;
    int arg;
};

static int ipc_server_fd = -1;
static struct wl_event_source *ipc_server_source = NULL;
static struct wl_event_loop *ipc_loop = NULL;
static int ipc_reserve_fd = -1;
static struct wl_list ipc_clients;
static struct wl_list ipc_pending_commands;
static bool ipc_initialized = false;
static char ipc_path[256];

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void set_cloexec(int fd) { fcntl(fd, F_SETFD, FD_CLOEXEC); }

static void set_nosigpipe(int fd) {
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
}

static const char *layout_name(enum Layout l) {
#define NAME(id, label, desc)                                                  \
    if ((l) == (id)) return (label);
    LAYOUTS_TABLE(NAME)
#undef NAME
    return "?";
}

static bool parse_layout(const char *s, enum Layout *out) {
#define PARSE(id, label, desc)                                                 \
    if (strcmp(s, label) == 0) {                                               \
        *out = (id);                                                           \
        return true;                                                           \
    }

    LAYOUTS_TABLE(PARSE)
#undef PARSE
    return false;
}

static bool parse_bool(const char *s, bool *out) {
    if (!strcmp(s, "1") || !strcmp(s, "true") || !strcmp(s, "on")
        || !strcmp(s, "yes")) {
        *out = true;
        return true;
    }

    if (!strcmp(s, "0") || !strcmp(s, "false") || !strcmp(s, "off")
        || !strcmp(s, "no")) {
        *out = false;
        return true;
    }

    return false;
}

static bool parse_int(const char *s, long *out) {
    if (!s || !*s) return false;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end) return false;
    *out = v;
    return true;
}

static bool parse_float(const char *s, float *out) {
    if (!s || !*s) return false;
    char *end = NULL;
    errno = 0;
    float v = strtof(s, &end);
    if (errno || end == s || *end) return false;
    *out = v;
    return true;
}

static uint32_t client_event_mask(const struct IpcClient *c) {
    uint32_t mask = WL_EVENT_READABLE;
    if (c->outlen > 0) mask |= WL_EVENT_WRITABLE;
    return mask;
}

static void client_update_events(struct IpcClient *c) {
    if (c->source == NULL) return;

    uint32_t want = client_event_mask(c);
    bool want_write = (want & WL_EVENT_WRITABLE) != 0;

    if (want_write == c->write_armed) return;

    if (wl_event_source_fd_update(c->source, want) < 0) {
        c->dead = true;
        return;
    }

    c->write_armed = want_write;
}

static void client_flush(struct IpcClient *c) {
    while (!c->dead && c->outlen > 0) {
        ssize_t n = send(c->fd, c->outbuf, c->outlen, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                client_update_events(c);
                return;
            }

            if (errno == EINTR) continue;
            c->dead = true;
            return;
        }

        if (n == 0) return;

        memmove(c->outbuf, c->outbuf + n, c->outlen - (size_t)n);
        c->outlen -= (size_t)n;
    }

    if (c->dead) return;

    client_update_events(c);

    if (c->read_closed && c->outlen == 0) c->dead = true;
}

static bool outbuf_reserve(struct IpcClient *c, size_t extra) {
    if (extra > IPC_OUTBUF_HARD) return false;
    if (c->outlen > IPC_OUTBUF_HARD - extra) return false;
    if (c->outlen + extra <= c->outcap) return true;

    size_t cap = c->outcap ? c->outcap : IPC_OUTBUF_SOFT;
    while (cap < c->outlen + extra) cap *= 2;
    if (cap > IPC_OUTBUF_HARD) return false;

    char *nb = realloc(c->outbuf, cap);
    if (!nb) return false;
    c->outbuf = nb;
    c->outcap = cap;
    return true;
}

static void outbuf_append(struct IpcClient *c, const char *data, size_t len) {
    if (c->dead) return;
    if (!outbuf_reserve(c, len)) {
        c->dead = true;
        return;
    }

    memcpy(c->outbuf + c->outlen, data, len);
    c->outlen += len;
    client_update_events(c);
}

static void outbuf_printf(struct IpcClient *c, const char *fmt, ...) {
    if (c->dead) return;
    char stackbuf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
    va_end(ap);

    if (n < 0) return;
    if ((size_t)n < sizeof(stackbuf)) {
        outbuf_append(c, stackbuf, (size_t)n);
        return;
    }

    char *big = malloc((size_t)n + 1);
    if (!big) {
        c->dead = true;
        return;
    }

    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    outbuf_append(c, big, (size_t)n);
    free(big);
}

static void outbuf_quoted(struct IpcClient *c, const char *s) {
    outbuf_append(c, "\"", 1);
    if (s) {
        for (const char *p = s; *p; p++) {
            switch (*p) {
                case '"': outbuf_append(c, "\\\"", 2); break;
                case '\\': outbuf_append(c, "\\\\", 2); break;
                case '\n': outbuf_append(c, "\\n", 2); break;
                case '\r': outbuf_append(c, "\\r", 2); break;
                case '\t': outbuf_append(c, "\\t", 2); break;
                default: outbuf_append(c, p, 1); break;
            }
        }
    }

    outbuf_append(c, "\"", 1);
}

static void client_destroy(struct IpcClient *c) {
    if (c->source != NULL) {
        wl_event_source_remove(c->source);
        c->source = NULL;
    }

    wl_list_remove(&c->link);
    close(c->fd);
    free(c->outbuf);
    free(c);
}

static void reap_dead_clients(void) {
    struct IpcClient *c, *tmp;
    wl_list_for_each_safe(c, tmp, &ipc_clients, link) {
        if (c->dead) client_destroy(c);
    }
}

static void resp_cfg_int(struct IpcClient *c, const char *key, long v) {
    outbuf_printf(c, "CFG %s %ld\n", key, v);
}

static void resp_cfg_float(struct IpcClient *c, const char *key, float v) {
    outbuf_printf(c, "CFG %s %g\n", key, (double)v);
}

static void resp_cfg_bool(struct IpcClient *c, const char *key, bool v) {
    outbuf_printf(c, "CFG %s %s\n", key, v ? "true" : "false");
}

static void resp_cfg_str(struct IpcClient *c, const char *key, const char *s) {
    outbuf_printf(c, "CFG %s ", key);
    outbuf_quoted(c, s);
    outbuf_append(c, "\n", 1);
}

static void resp_ok(struct IpcClient *c) { outbuf_append(c, "OK\n", 3); }

static void resp_ok_int(struct IpcClient *c, long v) {
    outbuf_printf(c, "OK %ld\n", v);
}

static void resp_ok_float(struct IpcClient *c, float v) {
    outbuf_printf(c, "OK %g\n", (double)v);
}

static void resp_ok_bool(struct IpcClient *c, bool v) {
    outbuf_append(c, v ? "OK true\n" : "OK false\n", v ? 8 : 9);
}

static void resp_ok_str(struct IpcClient *c, const char *s) {
    outbuf_append(c, "OK ", 3);
    outbuf_quoted(c, s);
    outbuf_append(c, "\n", 1);
}

static void resp_err(struct IpcClient *c, const char *msg) {
    outbuf_printf(c, "ERR %s\n", msg);
}

static void broadcast(const char *line, size_t len) {
    struct IpcClient *c;
    wl_list_for_each(c, &ipc_clients, link) {
        if (!c->subscribed || c->dead) continue;
        outbuf_append(c, line, len);
    }
}

static void broadcast_printf(const char *fmt, ...) {
    char stackbuf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
    va_end(ap);

    if (n < 0) return;
    if ((size_t)n < sizeof(stackbuf)) {
        broadcast(stackbuf, (size_t)n);
        return;
    }

    char *big = malloc((size_t)n + 1);
    if (!big) return;

    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    broadcast(big, (size_t)n);
    free(big);
}

static uint64_t window_id(const struct Window *w) {
    return (uint64_t)(uintptr_t)w;
}

static void emit_str(struct IpcClient *c, const char *key, const char *s) {
    outbuf_printf(c, "EVT %s ", key);
    outbuf_quoted(c, s);
    outbuf_append(c, "\n", 1);
}

static void emit_config(struct IpcClient *c) {
    emit_str(c, "layout", layout_name(wm.layout));

    outbuf_printf(c, "EVT current_space %d\n", wm.current_space);
    outbuf_printf(c, "EVT space_count %d\n", SPACE_COUNT);
    outbuf_printf(c, "EVT mfact %g\n", (double)wm.mfact);

#ifdef WALLPAPER
    emit_str(c, "wallpaper_path", wm.wallpaper.path ? wm.wallpaper.path : "");
#endif

#define IPC_KEY_INT(name, access, after, desc, field, min, max)                \
    outbuf_printf(c, "EVT %s %ld\n", name, (long)(field));
#define IPC_KEY_FLOAT(name, access, after, desc, field, min, max)              \
    outbuf_printf(c, "EVT %s %g\n", name, (double)(field));
#define IPC_KEY_BOOL(name, access, after, range, desc, field)                  \
    outbuf_printf(c, "EVT %s %s\n", name, (field) ? "true" : "false");
#define IPC_KEY_STR(name, access, after, range, desc, field)                   \
    emit_str(c, name, (field) != NULL ? (field) : "");
#define IPC_KEY_SPECIAL(name, access, range, desc)

    KEYS_TABLE(
        IPC_KEY_INT, IPC_KEY_FLOAT, IPC_KEY_BOOL, IPC_KEY_STR, IPC_KEY_SPECIAL
    )
    KEYS_TABLE_WALLPAPER(
        IPC_KEY_INT, IPC_KEY_FLOAT, IPC_KEY_BOOL, IPC_KEY_STR, IPC_KEY_SPECIAL
    )

#undef IPC_KEY_INT
#undef IPC_KEY_FLOAT
#undef IPC_KEY_BOOL
#undef IPC_KEY_STR
#undef IPC_KEY_SPECIAL
}

static void emit_spaces(struct IpcClient *c) {
    for (int s = 0; s < SPACE_COUNT; s++) {
        int n = 0;
        struct Window *w;
        wl_list_for_each(w, &wm.windows, link) {
            if (w->closed || w->space != s) continue;
            n++;
        }
        outbuf_printf(c, "EVT space_window_count %d %d\n", s, n);
    }
}

static void emit_focus(struct IpcClient *c) {
    struct Window *w = view_focused();
    if (w) {
        outbuf_printf(
            c, "EVT focused %llu\n", (unsigned long long)window_id(w)
        );

        emit_str(c, "focused_title", w->title ? w->title : "");
        emit_str(c, "focused_app_id", w->app_id ? w->app_id : "");
    } else {
        outbuf_printf(c, "EVT focused 0\n");
        emit_str(c, "focused_title", "");
        emit_str(c, "focused_app_id", "");
    }
}

static void emit_window(struct IpcClient *c, const struct Window *w) {
    outbuf_printf(
        c, "EVT window %llu space %d title ", (unsigned long long)window_id(w),
        w->space
    );
    outbuf_quoted(c, w->title ? w->title : "");
    outbuf_append(c, " app_id ", 8);
    outbuf_quoted(c, w->app_id ? w->app_id : "");
    outbuf_append(c, "\n", 1);
}

static void emit_all_state(struct IpcClient *c) {
    emit_config(c);
    emit_spaces(c);
    emit_focus(c);

    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->closed) continue;
        emit_window(c, w);
    }

    outbuf_append(c, "EVT ready\n", 10);
}

static bool ipc_queue_command(enum IpcCommandKind kind, int arg) {
    struct IpcCommand *cmd = calloc(1, sizeof(*cmd));
    if (cmd == NULL) return false;

    cmd->kind = kind;
    cmd->arg = arg;
    wl_list_insert(ipc_pending_commands.prev, &cmd->link);
    return true;
}

static bool
queue_or_err(struct IpcClient *c, enum IpcCommandKind kind, int arg) {
    if (server.locked) {
        resp_err(c, "SESSION LOCKED");
        return false;
    }

    if (!ipc_queue_command(kind, arg)) {
        resp_err(c, "OOM");
        return false;
    }

    return true;
}

void ipc_process_pending_commands(void) {
    if (!ipc_initialized) return;

    struct IpcCommand *cmd, *tmp;
    wl_list_for_each_safe(cmd, tmp, &ipc_pending_commands, link) {
        wl_list_remove(&cmd->link);

        if (server.locked) { // drop
            free(cmd);
            continue;
        }

        switch (cmd->kind) {
            case IPC_CMD_SWITCH_SPACE: wm_switch_space(cmd->arg); break;
            case IPC_CMD_MOVE_TO_SPACE:
                wm_move_window_to_space(view_focused(), cmd->arg);
                break;
            case IPC_CMD_SET_LAYOUT:
                wm_set_layout((enum Layout)cmd->arg);
                break;
            case IPC_CMD_CYCLE_LAYOUT:
                wm_set_layout(
                    (enum Layout)((wm.layout + 1) % ((int)LAYOUT_LAST + 1))
                );

                break;
            case IPC_CMD_CLOSE:
                {
                    struct Window *w = view_focused();
                    if (w != NULL) view_close(w);
                    break;
                }
        }

        free(cmd);
    }
}

void ipc_flush_pending(void) {
    if (!ipc_initialized) return;

    struct IpcClient *c;
    wl_list_for_each(c, &ipc_clients, link) {
        if (c->dead) continue;
        if (c->outlen > 0) client_flush(c);
    }

    reap_dead_clients();
}

void ipc_notify_layout(void) {
    if (!ipc_initialized) return;
    broadcast_printf("EVT layout %s\n", layout_name(wm.layout));
}

void ipc_notify_space(void) {
    if (!ipc_initialized) return;
    broadcast_printf("EVT current_space %d\n", wm.current_space);

    for (int s = 0; s < SPACE_COUNT; s++) {
        int n = 0;
        struct Window *w;
        wl_list_for_each(w, &wm.windows, link) {
            if (w->closed || w->space != s) continue;
            n++;
        }
        broadcast_printf("EVT space_window_count %d %d\n", s, n);
    }
}

void ipc_notify_focus(void) {
    if (!ipc_initialized) return;

    struct Window *w = view_focused();
    struct IpcClient *c;

    wl_list_for_each(c, &ipc_clients, link) {
        if (!c->subscribed || c->dead) continue;

        if (w) {
            outbuf_printf(
                c, "EVT focused %llu\n", (unsigned long long)window_id(w)
            );

            emit_str(c, "focused_title", w->title ? w->title : "");
            emit_str(c, "focused_app_id", w->app_id ? w->app_id : "");
        } else {
            outbuf_append(c, "EVT focused 0\n", 14);
            emit_str(c, "focused_title", "");
            emit_str(c, "focused_app_id", "");
        }
    }
}

void ipc_notify_kb_layout(void) {
    if (!ipc_initialized) return;

    struct IpcClient *c;
    wl_list_for_each(c, &ipc_clients, link) {
        if (!c->subscribed || c->dead) continue;
        emit_str(c, "kb_layout", wm.kb_layout ? wm.kb_layout : "");
    }
}

void ipc_notify_window_opened(struct Window *w) {
    if (!ipc_initialized || !w) return;

    broadcast_printf(
        "EVT window_opened %llu\n", (unsigned long long)window_id(w)
    );

    struct IpcClient *c;
    wl_list_for_each(c, &ipc_clients, link) {
        if (!c->subscribed || c->dead) continue;
        emit_window(c, w);
    }

    ipc_notify_space();
    ipc_notify_focus();
}

void ipc_notify_window_closed(struct Window *w) {
    if (!ipc_initialized || !w) return;

    broadcast_printf(
        "EVT window_closed %llu\n", (unsigned long long)window_id(w)
    );

    ipc_notify_space();
    ipc_notify_focus();
}

void ipc_notify_window_meta(struct Window *w) {
    if (!ipc_initialized || !w) return;

    struct IpcClient *c;
    wl_list_for_each(c, &ipc_clients, link) {
        if (!c->subscribed || c->dead) continue;
        emit_window(c, w);
    }

    if (w == view_focused()) ipc_notify_focus();
}

void ipc_notify_all_state(void) {
    if (!ipc_initialized) return;

    struct IpcClient *c;
    wl_list_for_each(c, &ipc_clients, link) {
        if (!c->subscribed || c->dead) continue;
        emit_all_state(c);
    }
}

static void after_set(enum IpcAfter a) {
    switch (a) {
        case IPC_AFTER_NONE: break;
        case IPC_AFTER_MANAGE: wm_mark_layout_dirty(); break;
        case IPC_AFTER_LIBINPUT: libinput_reconfigure(); break;
        case IPC_AFTER_WALLPAPER:
#ifdef WALLPAPER
            wm_invalidate_wallpaper();
#endif
            wm_mark_layout_dirty();
            break;
    }
}

static void handle_config(struct IpcClient *c) {
    resp_cfg_str(c, "layout", layout_name(wm.layout));
    resp_cfg_int(c, "current_space", wm.current_space);
    resp_cfg_int(c, "space_count", SPACE_COUNT);
    resp_cfg_float(c, "mfact", wm.mfact);
#ifdef WALLPAPER
    resp_cfg_str(
        c, "wallpaper_path", wm.wallpaper.path ? wm.wallpaper.path : ""
    );
#endif

#define IPC_KEY_INT(name, access, after, desc, field, min, max)                \
    if ((access) & IPC_A_R) resp_cfg_int(c, name, (long)(field));
#define IPC_KEY_FLOAT(name, access, after, desc, field, min, max)              \
    if ((access) & IPC_A_R) resp_cfg_float(c, name, (float)(field));
#define IPC_KEY_BOOL(name, access, after, range, desc, field)                  \
    if ((access) & IPC_A_R) resp_cfg_bool(c, name, (field));
#define IPC_KEY_STR(name, access, after, range, desc, field)                   \
    if ((access) & IPC_A_R)                                                    \
        resp_cfg_str(c, name, (field) != NULL ? (field) : "");

#define IPC_KEY_SPECIAL(name, access, range, desc)

    KEYS_TABLE(
        IPC_KEY_INT, IPC_KEY_FLOAT, IPC_KEY_BOOL, IPC_KEY_STR, IPC_KEY_SPECIAL
    )

    KEYS_TABLE_WALLPAPER(
        IPC_KEY_INT, IPC_KEY_FLOAT, IPC_KEY_BOOL, IPC_KEY_STR, IPC_KEY_SPECIAL
    )

#undef IPC_KEY_INT
#undef IPC_KEY_FLOAT
#undef IPC_KEY_BOOL
#undef IPC_KEY_STR
#undef IPC_KEY_SPECIAL

    resp_ok(c);
}

static void handle_get(struct IpcClient *c, const char *key, const char *arg) {
    if (key == NULL) {
        resp_err(c, "key is missing");
        return;
    }

#define IPC_KEY_INT(name, access, after, desc, field, min, max)                \
    if (((access) & IPC_A_R) != 0 && strcmp(key, name) == 0) {                 \
        resp_ok_int(c, (long)(field));                                         \
        return;                                                                \
    }

#define IPC_KEY_FLOAT(name, access, after, desc, field, min, max)              \
    if (((access) & IPC_A_R) != 0 && strcmp(key, name) == 0) {                 \
        resp_ok_float(c, (float)(field));                                      \
        return;                                                                \
    }

#define IPC_KEY_BOOL(name, access, after, range, desc, field)                  \
    if (((access) & IPC_A_R) != 0 && strcmp(key, name) == 0) {                 \
        resp_ok_bool(c, (field));                                              \
        return;                                                                \
    }

#define IPC_KEY_STR(name, access, after, range, desc, field)                   \
    if (((access) & IPC_A_R) != 0 && strcmp(key, name) == 0) {                 \
        const char *s = (field);                                               \
        resp_ok_str(c, s != NULL ? s : "");                                    \
        return;                                                                \
    }

#define IPC_KEY_SPECIAL(name, access, range, desc)

    KEYS_TABLE(
        IPC_KEY_INT, IPC_KEY_FLOAT, IPC_KEY_BOOL, IPC_KEY_STR, IPC_KEY_SPECIAL
    )

    KEYS_TABLE_WALLPAPER(
        IPC_KEY_INT, IPC_KEY_FLOAT, IPC_KEY_BOOL, IPC_KEY_STR, IPC_KEY_SPECIAL
    )

#undef IPC_KEY_INT
#undef IPC_KEY_FLOAT
#undef IPC_KEY_BOOL
#undef IPC_KEY_STR
#undef IPC_KEY_SPECIAL

    if (!strcmp(key, "layout")) {
        resp_ok_str(c, layout_name(wm.layout));
        return;
    }

    if (!strcmp(key, "current_space")) {
        resp_ok_int(c, wm.current_space);
        return;
    }

    if (!strcmp(key, "space_count")) {
        resp_ok_int(c, SPACE_COUNT);
        return;
    }

    if (!strcmp(key, "mfact")) {
        resp_ok_float(c, wm.mfact);
        return;
    }

    if (!strcmp(key, "focused_title")) {
        struct Window *w = view_focused();
        resp_ok_str(c, w && w->title ? w->title : "");
        return;
    }

    if (!strcmp(key, "focused_app_id")) {
        struct Window *w = view_focused();
        resp_ok_str(c, w && w->app_id ? w->app_id : "");
        return;
    }

    if (!strcmp(key, "space_window_count")) {
        long s;
        if (!parse_int(arg, &s) || s < 0 || s >= SPACE_COUNT) {
            resp_err(c, "bad space");
            return;
        }

        int n = 0;
        struct Window *w;
        wl_list_for_each(w, &wm.windows, link) {
            if (w->closed || w->space != (int)s) continue;
            n++;
        }

        resp_ok_int(c, n);
        return;
    }

#ifdef WALLPAPER
    if (!strcmp(key, "wallpaper_path")) {
        resp_ok_str(c, wm.wallpaper.path ? wm.wallpaper.path : "");
        return;
    }
#endif

    resp_err(c, "unknown key");
}

static void
handle_set(struct IpcClient *c, const char *key, const char *value) {
    if (key == NULL || value == NULL) {
        resp_err(c, "missing key/value");
        return;
    }

#define IPC_KEY_INT(name, access, after, desc, field, min, max)                \
    if (((access) & IPC_A_W) != 0 && strcmp(key, name) == 0) {                 \
        long v;                                                                \
        if (!parse_int(value, &v) || v < (min) || v > (max)) {                 \
            resp_err(c, "bad value");                                          \
            return;                                                            \
        }                                                                      \
        (field) = (int32_t)v;                                                  \
        broadcast_printf("EVT %s %ld\n", name, v);                             \
        after_set(after);                                                      \
        resp_ok(c);                                                            \
        return;                                                                \
    }

#define IPC_KEY_FLOAT(name, access, after, desc, field, min, max)              \
    if (((access) & IPC_A_W) != 0 && strcmp(key, name) == 0) {                 \
        float v;                                                               \
        if (!parse_float(value, &v) || v < (min) || v > (max)) {               \
            resp_err(c, "bad value");                                          \
            return;                                                            \
        }                                                                      \
        (field) = v;                                                           \
        broadcast_printf("EVT %s %g\n", name, (double)v);                      \
        after_set(after);                                                      \
        resp_ok(c);                                                            \
        return;                                                                \
    }

#define IPC_KEY_BOOL(name, access, after, range, desc, field)                  \
    if (((access) & IPC_A_W) != 0 && strcmp(key, name) == 0) {                 \
        bool b;                                                                \
        if (!parse_bool(value, &b)) {                                          \
            resp_err(c, "bad bool");                                           \
            return;                                                            \
        }                                                                      \
        (field) = b;                                                           \
        broadcast_printf("EVT %s %s\n", name, b ? "true" : "false");           \
        after_set(after);                                                      \
        resp_ok(c);                                                            \
        return;                                                                \
    }

#define IPC_KEY_STR(name, access, after, range, desc, field)
#define IPC_KEY_SPECIAL(name, access, range, desc)

    KEYS_TABLE(
        IPC_KEY_INT, IPC_KEY_FLOAT, IPC_KEY_BOOL, IPC_KEY_STR, IPC_KEY_SPECIAL
    )

    KEYS_TABLE_WALLPAPER(
        IPC_KEY_INT, IPC_KEY_FLOAT, IPC_KEY_BOOL, IPC_KEY_STR, IPC_KEY_SPECIAL
    )

#undef IPC_KEY_INT
#undef IPC_KEY_FLOAT
#undef IPC_KEY_BOOL
#undef IPC_KEY_STR
#undef IPC_KEY_SPECIAL

    if (!strcmp(key, "layout")) {
        enum Layout l;
        if (!parse_layout(value, &l)) {
            resp_err(c, "bad layout");
            return;
        }

        if (!queue_or_err(c, IPC_CMD_SET_LAYOUT, (int)l)) return;

        resp_ok(c);
        return;
    }

    if (!strcmp(key, "current_space")) {
        long v;
        if (!parse_int(value, &v) || v < 0 || v >= SPACE_COUNT) {
            resp_err(c, "bad space");
            return;
        }

        if (!queue_or_err(c, IPC_CMD_SWITCH_SPACE, (int)v)) return;

        resp_ok(c);
        return;
    }

    if (!strcmp(key, "mfact")) {
        float v;
        if (!parse_float(value, &v) || v <= 0.0f || v >= 1.0f) {
            resp_err(c, "bad value");
            return;
        }

        wm.mfact = v;
        broadcast_printf("EVT mfact %g\n", (double)wm.mfact);

        resp_ok(c);
        return;
    }

#ifdef WALLPAPER
    if (!strcmp(key, "wallpaper_path")) {
        if (value[0] == '\0') {
            resp_err(c, "bad value");
            return;
        }

        if (!wm_set_wallpaper_path(value)) {
            resp_err(c, "could not load");
            return;
        }

        struct IpcClient *it;
        wl_list_for_each(it, &ipc_clients, link) {
            if (!it->subscribed || it->dead) continue;
            emit_str(
                it, "wallpaper_path", wm.wallpaper.path ? wm.wallpaper.path : ""
            );
        }

        resp_ok(c);
        return;
    }
#endif

    resp_err(c, "unknown/read-only key");
}

static void dispatch_line(struct IpcClient *c, char *line) {
    while (*line == ' ' || *line == '\t') line++;
    size_t l = strlen(line);

    while (l > 0
           && (line[l - 1] == '\r' || line[l - 1] == '\n' || line[l - 1] == ' '
               || line[l - 1] == '\t')) {
        line[--l] = '\0';
    }

    if (l == 0) return;

    char *cmd = line;
    char *rest = strpbrk(cmd, " \t");
    if (rest) {
        *rest++ = '\0';
        while (*rest == ' ' || *rest == '\t') rest++;
    }

    if (!strcmp(cmd, "CONFIG")) {
        handle_config(c);
        return;
    }

    if (!strcmp(cmd, "PING")) {
        resp_ok(c);
        return;
    }

    if (!strcmp(cmd, "QUIT")) {
        c->dead = true;
        return;
    }

    if (!strcmp(cmd, "SUBSCRIBE")) {
        resp_ok(c);
        emit_all_state(c);
        c->subscribed = true;
        return;
    }

    if (!strcmp(cmd, "UNSUBSCRIBE")) {
        c->subscribed = false;
        resp_ok(c);
        return;
    }

    if (!strcmp(cmd, "GET")) {
        if (!rest) {
            resp_err(c, "key is missing");
            return;
        }

        char *arg = strpbrk(rest, " \t");
        if (arg) {
            *arg++ = '\0';
            while (*arg == ' ' || *arg == '\t') arg++;
        }

        handle_get(c, rest, arg);
        return;
    }

    if (!strcmp(cmd, "SET")) {
        if (!rest) {
            resp_err(c, "key is missing");
            return;
        }

        char *val = strpbrk(rest, " \t");
        if (!val) {
            resp_err(c, "value is missing");
            return;
        }

        *val++ = '\0';
        while (*val == ' ' || *val == '\t') val++;
        handle_set(c, rest, val);
        return;
    }

    if (!strcmp(cmd, "SWITCH_SPACE")) {
        long v;

        if (!parse_int(rest, &v) || v < 0 || v >= SPACE_COUNT) {
            resp_err(c, "bad space");
            return;
        }

        if (!queue_or_err(c, IPC_CMD_SWITCH_SPACE, (int)v)) return;

        resp_ok(c);
        return;
    }

    if (!strcmp(cmd, "MOVE_TO_SPACE")) {
        long v;

        if (!parse_int(rest, &v) || v < 0 || v >= SPACE_COUNT) {
            resp_err(c, "bad space");
            return;
        }

        if (!queue_or_err(c, IPC_CMD_MOVE_TO_SPACE, (int)v)) return;

        resp_ok(c);
        return;
    }

    if (!strcmp(cmd, "CYCLE_LAYOUT")) {
        if (!queue_or_err(c, IPC_CMD_CYCLE_LAYOUT, 0)) return;

        resp_ok(c);
        return;
    }

    if (!strcmp(cmd, "CLOSE")) {
        if (!queue_or_err(c, IPC_CMD_CLOSE, 0)) return;

        resp_ok(c);
        return;
    }

    if (!strcmp(cmd, "DPMS")) {
        if (!rest) {
            resp_err(c, "mode is missing");
            return;
        }

        char *name = strpbrk(rest, " \t");
        if (name) {
            *name++ = '\0';
            while (*name == ' ' || *name == '\t') name++;
            if (*name == '\0') name = NULL;
        }

        int mode;
        if (!strcmp(rest, "off"))
            mode = 0;
        else if (!strcmp(rest, "on"))
            mode = 1;
        else if (!strcmp(rest, "toggle"))
            mode = 2;
        else {
            resp_err(c, "bad mode (on|off|toggle)");
            return;
        }

        if (output_set_power(name, mode) == 0) {
            resp_err(c, name ? "no output" : "no outputs");
            return;
        }

        resp_ok(c);
        return;
    }

    resp_err(c, "unknown command");
}

static void client_read(struct IpcClient *c) {
    for (;;) {
        if (c->dead || c->read_closed) return;

        if (c->inlen == sizeof(c->inbuf)) {
            c->dead = true;
            return;
        }

        ssize_t n =
            recv(c->fd, c->inbuf + c->inlen, sizeof(c->inbuf) - c->inlen, 0);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            c->dead = true;
            return;
        }

        if (n == 0) {
            c->read_closed = true;
            if (c->outlen == 0) c->dead = true;
            return;
        }

        c->inlen += (size_t)n;

        size_t start = 0;
        for (size_t i = 0; i < c->inlen; i++) {
            if (c->inbuf[i] != '\n') continue;

            size_t len = i - start + 1;
            char tmp[IPC_INBUF_SIZE + 1];
            memcpy(tmp, c->inbuf + start, len);
            tmp[len] = '\0';
            dispatch_line(c, tmp);
            start = i + 1;

            if (c->dead) {
                c->inlen = 0;
                return;
            }
        }

        if (start > 0) {
            memmove(c->inbuf, c->inbuf + start, c->inlen - start);
            c->inlen -= start;
        }
    }
}

static int client_event_handler(int fd, uint32_t mask, void *data) {
    (void)fd;

    struct IpcClient *c = data;

    if (c->dead) {
        reap_dead_clients();
        return 0;
    }

    if (mask & WL_EVENT_ERROR) {
        c->dead = true;
        reap_dead_clients();
        return 0;
    }

    if (mask & WL_EVENT_HANGUP) { c->read_closed = true; }
    if (mask & WL_EVENT_READABLE) { client_read(c); }

    if (c->dead) {
        reap_dead_clients();
        return 0;
    }

    if (mask & WL_EVENT_WRITABLE) { client_flush(c); }
    if (c->dead) reap_dead_clients();

    return 0;
}

static void accept_client(void) {
    reap_dead_clients();

    for (;;) {
        int fd = accept(ipc_server_fd, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;

            if (errno == EMFILE || errno == ENFILE) {
                log_warn(
                    "Out of file descriptors (%s) (shedding connection)",
                    errno == EMFILE ? "EMFILE" : "ENFILE"
                );

                if (ipc_reserve_fd >= 0) {
                    close(ipc_reserve_fd);
                    ipc_reserve_fd = -1;

                    int shed = accept(ipc_server_fd, NULL, NULL);
                    if (shed >= 0) {
                        static const char msg[] = "ERR too many clients\n";
                        (void)send(shed, msg, sizeof(msg) - 1, 0);
                        close(shed);
                    }

                    ipc_reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
                }
                return;
            }

            log_warn("accept() failed: %s", strerror(errno));
            return;
        }

        set_nonblock(fd);
        set_cloexec(fd);
        set_nosigpipe(fd);

        struct IpcClient *c = calloc(1, sizeof(*c));
        if (!c) {
            close(fd);
            return;
        }

        c->fd = fd;
        wl_list_insert(ipc_clients.prev, &c->link);

        if (ipc_loop != NULL) {
            c->source = wl_event_loop_add_fd(
                ipc_loop, fd, WL_EVENT_READABLE, client_event_handler, c
            );

            if (c->source == NULL) c->dead = true;
        }
    }
}

static int server_event_handler(int fd, uint32_t mask, void *data) {
    (void)fd;
    (void)data;

    if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
        log_warn("IPC server socket error/hangup");
        return 0;
    }

    if (mask & WL_EVENT_READABLE) accept_client();
    return 0;
}

static int make_socket(void) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime) return -1;

    snprintf(
        ipc_path, sizeof(ipc_path), "%s/nullspace-%u.sock", runtime,
        (unsigned)getpid()
    );

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(ipc_path) >= sizeof(addr.sun_path)) {
        log_warn("Socket path too long: %s", ipc_path);
        return -1;
    }

    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc_path);

    signal(SIGPIPE, SIG_IGN);

    ipc_reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (ipc_reserve_fd < 0) {
        log_warn("Reserve open(/dev/null) failed: %s", strerror(errno));
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_warn("socket() failed: %s", strerror(errno));
        if (ipc_reserve_fd >= 0) {
            close(ipc_reserve_fd);
            ipc_reserve_fd = -1;
        }

        return -1;
    }

    set_nonblock(fd);
    set_cloexec(fd);
    set_nosigpipe(fd);

    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0) {
        set_cloexec(probe);
        if (connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            close(probe);
            close(fd);
            if (ipc_reserve_fd >= 0) {
                close(ipc_reserve_fd);
                ipc_reserve_fd = -1;
            }

            log_warn("Another instance is already listening on %s", ipc_path);
            return -1;
        }

        close(probe);
    }

    unlink(ipc_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_warn("bind(%s) failed: %s", ipc_path, strerror(errno));
        close(fd);
        if (ipc_reserve_fd >= 0) {
            close(ipc_reserve_fd);
            ipc_reserve_fd = -1;
        }

        return -1;
    }

    if (chmod(ipc_path, 0600) < 0) {
        log_warn("chmod(%s) failed: %s", ipc_path, strerror(errno));
    }

    if (listen(fd, SOMAXCONN) < 0) {
        log_warn("listen() failed: %s", strerror(errno));
        close(fd);
        unlink(ipc_path);
        if (ipc_reserve_fd >= 0) {
            close(ipc_reserve_fd);
            ipc_reserve_fd = -1;
        }

        return -1;
    }

    setenv("NULLSPACE_INSTANCE_SIGNATURE", ipc_path, 1);

    return fd;
}

void ipc_attach_event_loop(struct wl_event_loop *loop) {
    ipc_loop = loop;

    if (ipc_server_fd >= 0 && ipc_server_source == NULL) {
        ipc_server_source = wl_event_loop_add_fd(
            loop, ipc_server_fd, WL_EVENT_READABLE, server_event_handler, NULL
        );

        if (ipc_server_source == NULL) {
            log_warn("Failed to register socket");
        }
    }

    struct IpcClient *c;
    wl_list_for_each(c, &ipc_clients, link) {
        if (c->source != NULL) continue;
        c->source = wl_event_loop_add_fd(
            loop, c->fd, WL_EVENT_READABLE, client_event_handler, c
        );

        if (c->source == NULL) c->dead = true;
    }

    reap_dead_clients();
}

void ipc_init(void) {
    if (ipc_initialized) return;

    wl_list_init(&ipc_clients);
    wl_list_init(&ipc_pending_commands);

    ipc_server_fd = make_socket();
    if (ipc_server_fd < 0) {
        log_warn("Socket unavailable");
        ipc_server_fd = -1;
        return;
    }

    ipc_initialized = true;
    log_info("Listening on '%s'", ipc_path);

    if (ipc_loop != NULL) {
        ipc_server_source = wl_event_loop_add_fd(
            ipc_loop, ipc_server_fd, WL_EVENT_READABLE, server_event_handler,
            NULL
        );
    }
}

void ipc_destroy(void) {
    if (!ipc_initialized) return;

    unsetenv("NULLSPACE_INSTANCE_SIGNATURE");

    struct IpcClient *c, *tmp;
    wl_list_for_each_safe(c, tmp, &ipc_clients, link) { client_destroy(c); }

    struct IpcCommand *cmd, *cmdtmp;
    wl_list_for_each_safe(cmd, cmdtmp, &ipc_pending_commands, link) {
        wl_list_remove(&cmd->link);
        free(cmd);
    }

    if (ipc_server_source != NULL) {
        wl_event_source_remove(ipc_server_source);
        ipc_server_source = NULL;
    }

    if (ipc_reserve_fd >= 0) {
        close(ipc_reserve_fd);
        ipc_reserve_fd = -1;
    }

    if (ipc_server_fd >= 0) close(ipc_server_fd);
    ipc_server_fd = -1;

    if (ipc_path[0]) unlink(ipc_path);
    ipc_path[0] = '\0';
    ipc_initialized = false;
    ipc_loop = NULL;
}
