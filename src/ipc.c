#ifndef __BSD_VISIBLE
#define __BSD_VISIBLE 1
#endif

#include "ipc.h"

#include "input.h"
#include "layouts/layout.h"
#include "manage.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-util.h>

#define IPC_INBUF_SIZE  4096
#define IPC_OUTBUF_SOFT 4096
#define IPC_OUTBUF_HARD (256 * 1024)

struct IpcClient {
    int fd;
    bool subscribed;
    bool dead;
    bool read_closed;
    bool write_armed;
    char inbuf[IPC_INBUF_SIZE];
    size_t inlen;
    char *outbuf;
    size_t outlen;
    size_t outcap;
    struct wl_list link;
};

enum IpcCommandKind {
    IPC_CMD_SWITCH_SPACE,
    IPC_CMD_MOVE_TO_SPACE,
    IPC_CMD_SET_LAYOUT,
    IPC_CMD_CYCLE_LAYOUT,
    IPC_CMD_CLOSE,
};

struct IpcCommand {
    enum IpcCommandKind kind;
    int arg;
    struct wl_list link;
};

static int ipc_server_fd = -1;
static int ipc_reserve_fd = -1;
static int ipc_kq = -1;
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
    switch (l) {
        case LAYOUT_VERTICAL_TILE: return "vtile";
        case LAYOUT_VERTICAL_GRID: return "vgrid";
        case LAYOUT_HORIZONTAL_TILE: return "htile";
        case LAYOUT_HORIZONTAL_RIGHT_TILE: return "hrtile";
        case LAYOUT_HORIZONTAL_MONOCLE: return "monocle";
        case LAYOUT_HORIZONTAL_GRID: return "hgrid";
        case LAYOUT_FLOATING: return "float";
    }

    return "?";
}

static bool parse_layout(const char *s, enum Layout *out) {
    for (int i = 0; i <= (int)LAYOUT_LAST; i++) {
        const char *n = layout_name((enum Layout)i);
        if (strcmp(n, "?") == 0) continue;
        if (strcmp(s, n) == 0) {
            *out = (enum Layout)i;
            return true;
        }
    }

    if (strcmp(s, "floating") == 0) {
        *out = LAYOUT_FLOATING;
        return true;
    }

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

static void client_arm_write(struct IpcClient *c, bool arm) {
    if (ipc_kq < 0) return;
    if (c->write_armed == arm) return;

    struct kevent ev;

    EV_SET(
        &ev, (uintptr_t)c->fd, EVFILT_WRITE, arm ? EV_ADD : EV_DELETE, 0, 0,
        NULL
    );

    if (kevent(ipc_kq, &ev, 1, NULL, 0, NULL) < 0) {
        if (errno != ENOENT) c->dead = true;
        return;
    }

    c->write_armed = arm;
}

static void client_disarm_read(struct IpcClient *c) {
    if (ipc_kq < 0) return;
    struct kevent ev;
    EV_SET(&ev, (uintptr_t)c->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(ipc_kq, &ev, 1, NULL, 0, NULL);
}

static void client_flush(struct IpcClient *c) {
    while (!c->dead && c->outlen > 0) {
        ssize_t n = send(c->fd, c->outbuf, c->outlen, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                client_arm_write(c, true);
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
    client_arm_write(c, false);
    if (c->read_closed && c->outlen == 0) c->dead = true;
}

static void client_begin_close(struct IpcClient *c) {
    if (c->dead || c->read_closed) return;
    c->read_closed = true;
    client_disarm_read(c);

    if (c->outlen == 0) {
        c->dead = true;
        return;
    }

    client_flush(c);
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
    if (ipc_kq >= 0) {
        struct kevent ev;
        EV_SET(&ev, (uintptr_t)c->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        kevent(ipc_kq, &ev, 1, NULL, 0, NULL);

        if (c->write_armed) {
            EV_SET(&ev, (uintptr_t)c->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
            kevent(ipc_kq, &ev, 1, NULL, 0, NULL);
        }
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

static void emit_int(struct IpcClient *c, const char *key, long v) {
    outbuf_printf(c, "EVT %s %ld\n", key, v);
}

static void emit_float(struct IpcClient *c, const char *key, float v) {
    outbuf_printf(c, "EVT %s %g\n", key, (double)v);
}

static void emit_bool(struct IpcClient *c, const char *key, bool v) {
    outbuf_printf(c, "EVT %s %s\n", key, v ? "true" : "false");
}

static void emit_str(struct IpcClient *c, const char *key, const char *s) {
    outbuf_printf(c, "EVT %s ", key);
    outbuf_quoted(c, s);
    outbuf_append(c, "\n", 1);
}

static uint64_t window_id(const struct Window *w) {
    return (uint64_t)(uintptr_t)w;
}

static struct Window *focused_window(void) {
    struct Window *w;
    wl_list_for_each_reverse(w, &wm.focus_stack, focus_link) {
        if (w->closed || w->space_hidden) continue;
        return w;
    }

    return NULL;
}

static void emit_config(struct IpcClient *c) {
    emit_str(c, "layout", layout_name(wm.layout));
    emit_int(c, "current_space", wm.current_space);
    emit_int(c, "space_count", SPACE_COUNT);
    emit_str(c, "kb_layout", wm.kb_layout ? wm.kb_layout : "");

    emit_int(c, "nmasters", wm.nmasters);
    emit_float(c, "mfact", wm.mfact);

    emit_int(c, "gap_outer_h", wm.tiled_gap_outer_h);
    emit_int(c, "gap_outer_v", wm.tiled_gap_outer_v);
    emit_int(c, "gap_inner_h", wm.tiled_gap_inner_h);
    emit_int(c, "gap_inner_v", wm.tiled_gap_inner_v);
    emit_bool(c, "smart_gaps", wm.smart_gaps);

    emit_bool(c, "center_overspread", wm.center_overspread);
    emit_bool(c, "center_when_single_stack", wm.center_when_single_stack);

    emit_int(c, "animations_enabled", wm.animations.enabled);
    emit_int(c, "animations_open_from_top", wm.animations.open_from_top);
    emit_int(c, "animations_duration_move", wm.animations.duration_move);
    emit_int(c, "animations_duration_open", wm.animations.duration_open);
    emit_int(c, "animations_duration_close", wm.animations.duration_close);
    emit_int(c, "animations_duration_space", wm.animations.duration_space);

#ifdef WALLPAPER
    emit_int(c, "wallpaper_topbar_fade_h", wm.wallpaper.topbar_fade_h);
    emit_str(c, "wallpaper_path", wm.wallpaper.path ? wm.wallpaper.path : "");
    emit_int(c, "wallpaper_pattern_bg_r", wm.wallpaper.pattern_bg_r);
    emit_int(c, "wallpaper_pattern_bg_g", wm.wallpaper.pattern_bg_g);
    emit_int(c, "wallpaper_pattern_bg_b", wm.wallpaper.pattern_bg_b);
    emit_int(c, "wallpaper_pattern_dot_r", wm.wallpaper.pattern_dot_r);
    emit_int(c, "wallpaper_pattern_dot_g", wm.wallpaper.pattern_dot_g);
    emit_int(c, "wallpaper_pattern_dot_b", wm.wallpaper.pattern_dot_b);
    emit_int(
        c, "wallpaper_pattern_grid_spacing", wm.wallpaper.pattern_grid_spacing
    );
    emit_int(
        c, "wallpaper_pattern_dot_radius", wm.wallpaper.pattern_dot_radius
    );
    emit_int(c, "wallpaper_blur_radius", wm.wallpaper.blur_radius);
    emit_int(c, "wallpaper_blur_passes", wm.wallpaper.blur_passes);
    emit_int(c, "wallpaper_blur_top_inset", wm.wallpaper.blur_top_inset);
#endif

    emit_int(c, "libinput_tap_state", wm.libinput.tap_state);
    emit_int(c, "libinput_natural_scroll", wm.libinput.natural_scroll);
    emit_int(c, "libinput_left_handed", wm.libinput.left_handed);
    emit_int(c, "libinput_middle_emulation", wm.libinput.middle_emulation);
    emit_int(c, "libinput_dwt", wm.libinput.dwt);
    emit_int(c, "libinput_drag", wm.libinput.drag);
    emit_int(c, "libinput_drag_lock", wm.libinput.drag_lock);
    emit_int(c, "libinput_three_finger_drag", wm.libinput.three_finger_drag);
    emit_int(c, "libinput_accel_profile", wm.libinput.accel_profile);
    emit_float(c, "libinput_accel_speed", wm.libinput.accel_speed);
    emit_int(c, "libinput_click_method", wm.libinput.click_method);
    emit_int(c, "libinput_scroll_method", wm.libinput.scroll_method);
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
    struct Window *w = focused_window();
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

static void broadcast_emit_str(const char *key, const char *s) {
    struct IpcClient *c;
    wl_list_for_each(c, &ipc_clients, link) {
        if (!c->subscribed || c->dead) continue;
        emit_str(c, key, s);
    }
}

static void ipc_queue_command(enum IpcCommandKind kind, int arg) {
    struct IpcCommand *cmd = calloc(1, sizeof(*cmd));
    if (cmd == NULL) return;

    cmd->kind = kind;
    cmd->arg = arg;

    wl_list_insert(ipc_pending_commands.prev, &cmd->link);
}

void ipc_process_pending_commands(void) {
    if (!ipc_initialized) return;

    struct IpcCommand *cmd, *tmp;
    wl_list_for_each_safe(cmd, tmp, &ipc_pending_commands, link) {
        wl_list_remove(&cmd->link);

        switch (cmd->kind) {
            case IPC_CMD_SWITCH_SPACE: wm_switch_space(cmd->arg); break;

            case IPC_CMD_MOVE_TO_SPACE:
                {
                    struct Seat *seat;
                    wl_list_for_each(seat, &wm.seats, link) {
                        wm_move_window_to_space(seat, cmd->arg);
                    }

                    break;
                }
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
                    struct Window *w = focused_window();
                    if (w != NULL) river_window_v1_close(w->obj);
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
    struct Window *w = focused_window();
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
    broadcast_emit_str("kb_layout", wm.kb_layout ? wm.kb_layout : "");
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

    if (w == focused_window()) ipc_notify_focus();
}

void ipc_notify_all_state(void) {
    if (!ipc_initialized) return;
    struct IpcClient *c;
    wl_list_for_each(c, &ipc_clients, link) {
        if (!c->subscribed || c->dead) continue;
        emit_all_state(c);
    }
}

static void handle_get(struct IpcClient *c, const char *key, const char *arg) {
    if (!key) {
        resp_err(c, "key missing");
        return;
    }

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

    if (!strcmp(key, "kb_layout")) {
        resp_ok_str(c, wm.kb_layout ? wm.kb_layout : "");
        return;
    }

    if (!strcmp(key, "nmasters")) {
        resp_ok_int(c, wm.nmasters);
        return;
    }

    if (!strcmp(key, "mfact")) {
        resp_ok_float(c, wm.mfact);
        return;
    }

    if (!strcmp(key, "smart_gaps")) {
        resp_ok_bool(c, wm.smart_gaps);
        return;
    }

    if (!strcmp(key, "center_overspread")) {
        resp_ok_bool(c, wm.center_overspread);
        return;
    }

    if (!strcmp(key, "center_when_single_stack")) {
        resp_ok_bool(c, wm.center_when_single_stack);
        return;
    }

    if (!strcmp(key, "gap_outer_h")) {
        resp_ok_int(c, wm.tiled_gap_outer_h);
        return;
    }

    if (!strcmp(key, "gap_outer_v")) {
        resp_ok_int(c, wm.tiled_gap_outer_v);
        return;
    }

    if (!strcmp(key, "gap_inner_h")) {
        resp_ok_int(c, wm.tiled_gap_inner_h);
        return;
    }

    if (!strcmp(key, "gap_inner_v")) {
        resp_ok_int(c, wm.tiled_gap_inner_v);
        return;
    }

    if (!strcmp(key, "animations_enabled")) {
        resp_ok_bool(c, wm.animations.enabled);
        return;
    }

    if (!strcmp(key, "animations_open_from_top")) {
        resp_ok_bool(c, wm.animations.open_from_top);
        return;
    }

    {
        struct {
            const char *key;
            int32_t value;
        } anims[] = {
            { "animations_duration_move",  wm.animations.duration_move},
            { "animations_duration_open",  wm.animations.duration_open},
            {"animations_duration_close", wm.animations.duration_close},
            {"animations_duration_space", wm.animations.duration_space},
        };

        for (size_t i = 0; i < sizeof(anims) / sizeof(anims[0]); i++) {
            if (!strcmp(key, anims[i].key)) {
                resp_ok_int(c, anims[i].value);
                return;
            }
        }
    }

#ifdef WALLPAPER
    if (!strcmp(key, "wallpaper_topbar_fade_h")) {
        resp_ok_int(c, wm.wallpaper.topbar_fade_h);
        return;
    }

    if (!strcmp(key, "wallpaper_path")) {
        resp_ok_str(c, wm.wallpaper.path ? wm.wallpaper.path : "");
        return;
    }
#endif

#ifdef WALLPAPER
    {
        struct {
            const char *key;
            int32_t value;
        } wps[] = {
            {        "wallpaper_pattern_bg_r",         wm.wallpaper.pattern_bg_r},
            {        "wallpaper_pattern_bg_g",         wm.wallpaper.pattern_bg_g},
            {        "wallpaper_pattern_bg_b",         wm.wallpaper.pattern_bg_b},
            {       "wallpaper_pattern_dot_r",        wm.wallpaper.pattern_dot_r},
            {       "wallpaper_pattern_dot_g",        wm.wallpaper.pattern_dot_g},
            {       "wallpaper_pattern_dot_b",        wm.wallpaper.pattern_dot_b},
            {"wallpaper_pattern_grid_spacing", wm.wallpaper.pattern_grid_spacing},
            {  "wallpaper_pattern_dot_radius",   wm.wallpaper.pattern_dot_radius},
            {         "wallpaper_blur_radius",          wm.wallpaper.blur_radius},
            {         "wallpaper_blur_passes",          wm.wallpaper.blur_passes},
            {      "wallpaper_blur_top_inset",       wm.wallpaper.blur_top_inset},
        };

        for (size_t i = 0; i < sizeof(wps) / sizeof(wps[0]); i++) {
            if (!strcmp(key, wps[i].key)) {
                resp_ok_int(c, wps[i].value);
                return;
            }
        }
    }
#endif

    {
        struct {
            const char *key;
            int32_t value;
        } lis[] = {
            {        "libinput_tap_state",         wm.libinput.tap_state},
            {   "libinput_natural_scroll",    wm.libinput.natural_scroll},
            {      "libinput_left_handed",       wm.libinput.left_handed},
            { "libinput_middle_emulation",  wm.libinput.middle_emulation},
            {              "libinput_dwt",               wm.libinput.dwt},
            {             "libinput_drag",              wm.libinput.drag},
            {        "libinput_drag_lock",         wm.libinput.drag_lock},
            {"libinput_three_finger_drag", wm.libinput.three_finger_drag},
            {    "libinput_accel_profile",     wm.libinput.accel_profile},
            {     "libinput_click_method",      wm.libinput.click_method},
            {    "libinput_scroll_method",     wm.libinput.scroll_method},
        };

        for (size_t i = 0; i < sizeof(lis) / sizeof(lis[0]); i++) {
            if (!strcmp(key, lis[i].key)) {
                resp_ok_int(c, lis[i].value);
                return;
            }
        }
    }

    if (!strcmp(key, "libinput_accel_speed")) {
        resp_ok_float(c, wm.libinput.accel_speed);
        return;
    }

    if (!strcmp(key, "focused_title")) {
        struct Window *w = focused_window();
        resp_ok_str(c, w && w->title ? w->title : "");
        return;
    }

    if (!strcmp(key, "focused_app_id")) {
        struct Window *w = focused_window();
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

    resp_err(c, "unknown key");
}

static void
handle_set(struct IpcClient *c, const char *key, const char *value) {
    if (!key || !value) {
        resp_err(c, "missing key/value");
        return;
    }

    if (!strcmp(key, "layout")) {
        enum Layout l;
        if (!parse_layout(value, &l)) {
            resp_err(c, "bad layout");
            return;
        }

        ipc_queue_command(IPC_CMD_SET_LAYOUT, (int)l);
        wm_request_manage();
        resp_ok(c);
        return;
    }

    if (!strcmp(key, "current_space")) {
        long v;
        if (!parse_int(value, &v) || v < 0 || v >= SPACE_COUNT) {
            resp_err(c, "bad space");
            return;
        }

        ipc_queue_command(IPC_CMD_SWITCH_SPACE, (int)v);
        wm_request_manage();
        resp_ok(c);
        return;
    }

    if (!strcmp(key, "nmasters")) {
        long v;
        if (!parse_int(value, &v) || v < 1 || v > 1024) {
            resp_err(c, "bad value");
            return;
        }
        wm.nmasters = (int32_t)v;
        broadcast_printf("EVT nmasters %d\n", wm.nmasters);
        wm_request_manage();
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
        wm_request_manage();
        resp_ok(c);
        return;
    }

    int32_t *slot = NULL;
    if (!strcmp(key, "gap_outer_h"))
        slot = &wm.tiled_gap_outer_h;
    else if (!strcmp(key, "gap_outer_v"))
        slot = &wm.tiled_gap_outer_v;
    else if (!strcmp(key, "gap_inner_h"))
        slot = &wm.tiled_gap_inner_h;
    else if (!strcmp(key, "gap_inner_v"))
        slot = &wm.tiled_gap_inner_v;
    if (slot) {
        long v;
        if (!parse_int(value, &v) || v < 0 || v > 500) {
            resp_err(c, "bad value");
            return;
        }

        *slot = (int32_t)v;
        broadcast_printf("EVT %s %d\n", key, *slot);
        wm_request_manage();
        resp_ok(c);
        return;
    }

    bool *bslot = NULL;
    if (!strcmp(key, "smart_gaps"))
        bslot = &wm.smart_gaps;
    else if (!strcmp(key, "center_overspread"))
        bslot = &wm.center_overspread;
    else if (!strcmp(key, "center_when_single_stack"))
        bslot = &wm.center_when_single_stack;
    else if (!strcmp(key, "animations_enabled"))
        bslot = &wm.animations.enabled;
    else if (!strcmp(key, "animations_open_from_top"))
        bslot = &wm.animations.open_from_top;
    if (bslot) {
        bool b;
        if (!parse_bool(value, &b)) {
            resp_err(c, "bad bool");
            return;
        }

        *bslot = b;
        broadcast_printf("EVT %s %s\n", key, b ? "true" : "false");
        wm_request_manage();
        resp_ok(c);
        return;
    }

    {
        struct {
            const char *key;
            int32_t *slot;
        } anims[] = {
            { "animations_duration_move",  &wm.animations.duration_move},
            { "animations_duration_open",  &wm.animations.duration_open},
            {"animations_duration_close", &wm.animations.duration_close},
            {"animations_duration_space", &wm.animations.duration_space},
        };

        for (size_t i = 0; i < sizeof(anims) / sizeof(anims[0]); i++) {
            if (strcmp(key, anims[i].key) != 0) { continue; }

            long v;
            if (!parse_int(value, &v) || v < 0 || v > 60000) {
                resp_err(c, "bad value");
                return;
            }

            *anims[i].slot = (int32_t)v;
            broadcast_printf("EVT %s %d\n", key, *anims[i].slot);
            wm_request_manage();
            resp_ok(c);
            return;
        }
    }

#ifdef WALLPAPER
    if (!strcmp(key, "wallpaper_topbar_fade_h")) {
        long v;
        if (!parse_int(value, &v) || v < 0 || v > 1000) {
            resp_err(c, "bad value");
            return;
        }

        wm.wallpaper.topbar_fade_h = (int32_t)v;
        broadcast_printf("EVT wallpaper_topbar_fade_h %d\n", (int32_t)v);
        resp_ok(c);
        return;
    }

    if (!strcmp(key, "wallpaper_path")) {
        if (value[0] == '\0') {
            resp_err(c, "bad value");
            return;
        }

        if (!wm_set_wallpaper_path(value)) {
            resp_err(c, "could not load");
            return;
        }

        broadcast_emit_str(
            "wallpaper_path", wm.wallpaper.path ? wm.wallpaper.path : ""
        );

        resp_ok(c);
        return;
    }

    {
        struct {
            const char *key;
            int32_t *slot;
            int32_t min, max;
        } wps[] = {
            {        "wallpaper_pattern_bg_r",&wm.wallpaper.pattern_bg_r,0, 255                                                                                },
            {        "wallpaper_pattern_bg_g",       &wm.wallpaper.pattern_bg_g, 0, 255},
            {        "wallpaper_pattern_bg_b",       &wm.wallpaper.pattern_bg_b, 0, 255},
            {       "wallpaper_pattern_dot_r",      &wm.wallpaper.pattern_dot_r, 0, 255},
            {       "wallpaper_pattern_dot_g",      &wm.wallpaper.pattern_dot_g, 0, 255},
            {       "wallpaper_pattern_dot_b",      &wm.wallpaper.pattern_dot_b, 0, 255},
            {"wallpaper_pattern_grid_spacing",
             &wm.wallpaper.pattern_grid_spacing, 1, 512                                },
            {  "wallpaper_pattern_dot_radius", &wm.wallpaper.pattern_dot_radius,
             0, 128                                                                    },
            {         "wallpaper_blur_radius",        &wm.wallpaper.blur_radius, 0, 256},
            {         "wallpaper_blur_passes",        &wm.wallpaper.blur_passes, 0,  16},
            {      "wallpaper_blur_top_inset",     &wm.wallpaper.blur_top_inset, 0, 512},
        };

        for (size_t i = 0; i < sizeof(wps) / sizeof(wps[0]); i++) {
            if (strcmp(key, wps[i].key) != 0) { continue; }

            long v;
            if (!parse_int(value, &v) || v < wps[i].min || v > wps[i].max) {
                resp_err(c, "bad value");
                return;
            }

            *wps[i].slot = (int32_t)v;
            broadcast_printf("EVT %s %d\n", key, *wps[i].slot);
            wm_invalidate_wallpaper();
            resp_ok(c);
            return;
        }
    }
#endif

    {
        struct {
            const char *key;
            int32_t *slot;
            int32_t min, max;
        } lis[] = {
            {        "libinput_tap_state",         &wm.libinput.tap_state, -1, 1},
            {   "libinput_natural_scroll",    &wm.libinput.natural_scroll, -1, 1},
            {      "libinput_left_handed",       &wm.libinput.left_handed, -1, 1},
            { "libinput_middle_emulation",  &wm.libinput.middle_emulation, -1, 1},
            {              "libinput_dwt",               &wm.libinput.dwt, -1, 1},
            {             "libinput_drag",              &wm.libinput.drag, -1, 1},
            {        "libinput_drag_lock",         &wm.libinput.drag_lock, -1, 2},
            {"libinput_three_finger_drag", &wm.libinput.three_finger_drag, -1, 2},
            {    "libinput_accel_profile",     &wm.libinput.accel_profile, -1, 7},
            {     "libinput_click_method",      &wm.libinput.click_method, -1, 2},
            {    "libinput_scroll_method",     &wm.libinput.scroll_method, -1, 7},
        };

        for (size_t i = 0; i < sizeof(lis) / sizeof(lis[0]); i++) {
            if (strcmp(key, lis[i].key) != 0) { continue; }

            long v;
            if (!parse_int(value, &v) || v < lis[i].min || v > lis[i].max) {
                resp_err(c, "bad value");
                return;
            }

            *lis[i].slot = (int32_t)v;
            broadcast_printf("EVT %s %d\n", key, *lis[i].slot);
            libinput_reconfigure();
            resp_ok(c);
            return;
        }
    }

    if (!strcmp(key, "libinput_accel_speed")) {
        float v;
        if (!parse_float(value, &v) || v < -2.0f || v > 1.0f) {
            resp_err(c, "bad value");
            return;
        }

        wm.libinput.accel_speed = v;
        broadcast_printf("EVT libinput_accel_speed %g\n", (double)v);
        libinput_reconfigure();
        resp_ok(c);
        return;
    }

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
            resp_err(c, "missing key");
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
            resp_err(c, "missing key");
            return;
        }

        char *val = strpbrk(rest, " \t");
        if (!val) {
            resp_err(c, "missing value");
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

        ipc_queue_command(IPC_CMD_SWITCH_SPACE, (int)v);
        wm_request_manage();
        resp_ok(c);
        return;
    }

    if (!strcmp(cmd, "MOVE_TO_SPACE")) {
        long v;

        if (!parse_int(rest, &v) || v < 0 || v >= SPACE_COUNT) {
            resp_err(c, "bad space");
            return;
        }

        ipc_queue_command(IPC_CMD_MOVE_TO_SPACE, (int)v);
        wm_request_manage();
        resp_ok(c);
        return;
    }

    if (!strcmp(cmd, "CYCLE_LAYOUT")) {
        ipc_queue_command(IPC_CMD_CYCLE_LAYOUT, 0);
        wm_request_manage();
        resp_ok(c);
        return;
    }

    if (!strcmp(cmd, "CLOSE")) {
        ipc_queue_command(IPC_CMD_CLOSE, 0);
        wm_request_manage();
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
            client_begin_close(c);
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

static void accept_client(void) {
    reap_dead_clients();

    for (;;) {
        int fd = accept(ipc_server_fd, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;

            if (errno == EMFILE || errno == ENFILE) {
                fprintf(
                    stderr,
                    "IPC: out of file descriptors (%s); shedding connection\n",
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

            fprintf(stderr, "IPC: accept() failed: %s\n", strerror(errno));
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

        if (ipc_kq >= 0) {
            struct kevent ev;
            EV_SET(&ev, (uintptr_t)fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
            if (kevent(ipc_kq, &ev, 1, NULL, 0, NULL) < 0) { c->dead = true; }
        }
    }
}

static void register_server_in_kq(void) {
    if (ipc_kq < 0 || ipc_server_fd < 0) return;

    struct kevent ev;
    EV_SET(&ev, (uintptr_t)ipc_server_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);

    if (kevent(ipc_kq, &ev, 1, NULL, 0, NULL) < 0) {
        fprintf(stderr, "IPC: kevent(server) failed: %s\n", strerror(errno));
    }
}

void ipc_kqueue_register(int kq) {
    ipc_kq = kq;
    register_server_in_kq();
}

void ipc_kqueue_handle(const struct kevent *ev) {
    int fd = (int)ev->ident;

    if (ev->filter == EVFILT_READ) {
        if (fd == ipc_server_fd) {
            accept_client();
            reap_dead_clients();
            return;
        }

        struct IpcClient *c = NULL, *it;
        wl_list_for_each(it, &ipc_clients, link) {
            if (it->fd == fd) {
                c = it;
                break;
            }
        }

        if (c == NULL) return;

        client_read(c);

        if (!c->dead && !c->read_closed && (ev->flags & EV_EOF)) {
            client_begin_close(c);
        }

        reap_dead_clients();
        return;
    }

    if (ev->filter == EVFILT_WRITE) {
        struct IpcClient *c = NULL, *it;
        wl_list_for_each(it, &ipc_clients, link) {
            if (it->fd == fd) {
                c = it;
                break;
            }
        }

        if (c == NULL) return;

        client_flush(c);
        reap_dead_clients();
        return;
    }
}

static int make_socket(void) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *display = getenv("WAYLAND_DISPLAY");

    if (runtime && *runtime) {
        if (display && *display) {
            snprintf(
                ipc_path, sizeof(ipc_path), "%s/nullspace-%s.sock", runtime,
                display
            );

        } else {
            snprintf(ipc_path, sizeof(ipc_path), "%s/nullspace.sock", runtime);
        }
    } else {
        snprintf(
            ipc_path, sizeof(ipc_path), "/tmp/nullspace-%u.sock",
            (unsigned)getuid()
        );
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(ipc_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "IPC: socket path too long: %s\n", ipc_path);
        return -1;
    }

    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc_path);

    signal(SIGPIPE, SIG_IGN);

    ipc_reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (ipc_reserve_fd < 0) {
        fprintf(
            stderr, "IPC: reserve open(/dev/null) failed: %s\n", strerror(errno)
        );
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "IPC: socket() failed: %s\n", strerror(errno));
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

            fprintf(
                stderr, "IPC: another instance is already listening on %s\n",
                ipc_path
            );

            return -1;
        }

        close(probe);
    }

    unlink(ipc_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(
            stderr, "IPC: bind(%s) failed: %s\n", ipc_path, strerror(errno)
        );

        close(fd);
        if (ipc_reserve_fd >= 0) {
            close(ipc_reserve_fd);
            ipc_reserve_fd = -1;
        }

        return -1;
    }

    if (chmod(ipc_path, 0600) < 0) {
        fprintf(
            stderr, "IPC: chmod(%s) failed: %s\n", ipc_path, strerror(errno)
        );
    }

    if (listen(fd, SOMAXCONN) < 0) {
        fprintf(stderr, "IPC: listen() failed: %s\n", strerror(errno));
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

void ipc_init(void) {
    if (ipc_initialized) return;
    wl_list_init(&ipc_clients);
    wl_list_init(&ipc_pending_commands);

    ipc_server_fd = make_socket();
    if (ipc_server_fd < 0) {
        fprintf(stderr, "IPC: disabled (socket unavailable)\n");
        ipc_server_fd = -1;
        return;
    }

    ipc_initialized = true;
    register_server_in_kq();
    fprintf(stderr, "IPC: listening on %s\n", ipc_path);
}

void ipc_destroy(void) {
    if (!ipc_initialized) return;

    unsetenv("NULLSPACE_INSTANCE_SIGNATURE");
    ipc_kq = -1;

    struct IpcClient *c, *tmp;
    wl_list_for_each_safe(c, tmp, &ipc_clients, link) { client_destroy(c); }

    struct IpcCommand *cmd, *cmdtmp;
    wl_list_for_each_safe(cmd, cmdtmp, &ipc_pending_commands, link) {
        wl_list_remove(&cmd->link);
        free(cmd);
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
}
