#include "ipc.h"

#include "input.h"
#include "layouts/layout.h"
#include "log.h"
#include "manager.h"
#include "output.h"
#include "server.h"
#include "window.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <ns-util.h>
#include <nullspace/keys.h>
#include <nullspace/layouts.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define IPC_LINE_MAX    (64 * 1024)
#define IPC_MAX_CLIENTS 64
#define IPC_PENDING_MAX 1024

#define ARRAY_LEN(a)    (sizeof(a) / sizeof((a)[0]))

struct IpcClient {
    struct wl_list link;
    struct wl_event_source *source;

    char inbuf[IPC_INBUF_SIZE];
    char *outbuf;

    size_t inlen;
    size_t outoff;
    size_t outlen;
    size_t outcap;

    uint32_t mask;
    int fd;

    bool subscribed;
    bool dead;
    bool read_closed;
};

typedef void (*IpcAction)(int arg);

struct IpcPending {
    struct wl_list link;
    IpcAction action;
    int arg;
};

static struct wl_list ipc_clients = {&ipc_clients, &ipc_clients};
static struct wl_list ipc_pending = {&ipc_pending, &ipc_pending};

static size_t ipc_client_count;
static size_t ipc_pending_count;

static int ipc_server_fd = -1;
static int ipc_reserve_fd = -1;
static struct wl_event_source *ipc_server_source;
static struct wl_event_loop *ipc_loop;
static bool ipc_initialized;
static char ipc_path[256];

static bool parse_bool(const char *s, bool *out) {
    static const char *const yes[] = {"1", "true", "on", "yes"};
    static const char *const no[] = {"0", "false", "off", "no"};

    for (size_t i = 0; i < ARRAY_LEN(yes); i++) {
        if (!strcmp(s, yes[i])) {
            *out = true;
            return true;
        }

        if (!strcmp(s, no[i])) {
            *out = false;
            return true;
        }
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
    if (errno || end == s || *end || !isfinite(v)) return false;
    *out = v;
    return true;
}

static bool parse_space(const char *s, int *out) {
    long v;
    if (!parse_int(s, &v) || v < 0 || v >= SPACE_COUNT) return false;
    *out = (int)v;
    return true;
}

static char *skip_ws(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static char *split_token(char *s) {
    char *p = strpbrk(s, " \t");
    if (!p) return NULL;
    *p++ = '\0';
    p = skip_ws(p);
    return *p ? p : NULL;
}

static const struct {
    enum Layout id;
    const char *name;
} layout_table[] = {
#define ROW(id, label, desc) {(id), (label)},
    LAYOUTS_TABLE(ROW)
#undef ROW
};

static const char *layout_name(enum Layout l) {
    for (size_t i = 0; i < ARRAY_LEN(layout_table); i++) {
        if (layout_table[i].id == l) return layout_table[i].name;
    }

    return "?";
}

static bool parse_layout(const char *s, enum Layout *out) {
    for (size_t i = 0; i < ARRAY_LEN(layout_table); i++) {
        if (!strcmp(s, layout_table[i].name)) {
            *out = layout_table[i].id;
            return true;
        }
    }

    return false;
}

static uint32_t client_wanted_mask(const struct IpcClient *c) {
    uint32_t m = 0;
    if (!c->read_closed) m |= WL_EVENT_READABLE;
    if (c->outlen > c->outoff) m |= WL_EVENT_WRITABLE;
    return m;
}

static void client_sync_events(struct IpcClient *c) {
    if (c->source == NULL || c->dead) return;

    uint32_t want = client_wanted_mask(c);
    if (want == c->mask) return;

    if (wl_event_source_fd_update(c->source, want) < 0) {
        c->dead = true;
        return;
    }

    c->mask = want;
}

static void client_flush(struct IpcClient *c) {
    while (!c->dead && c->outoff < c->outlen) {
        ssize_t n =
            send(c->fd, c->outbuf + c->outoff, c->outlen - c->outoff, 0);

        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) c->dead = true;
            break;
        }

        c->outoff += (size_t)n;
    }

    if (c->dead) return;

    if (c->outoff == c->outlen) {
        c->outoff = c->outlen = 0;

        if (c->outcap > IPC_OUTBUF_SOFT) {
            free(c->outbuf);
            c->outbuf = NULL;
            c->outcap = 0;
        }

        if (c->read_closed) {
            c->dead = true;
            return;
        }
    }

    client_sync_events(c);
}

static void outbuf_append(struct IpcClient *c, const char *data, size_t len) {
    if (c->dead || len == 0) return;

    size_t pending = c->outlen - c->outoff;
    if (len > IPC_OUTBUF_HARD || pending > IPC_OUTBUF_HARD - len) {
        c->dead = true;
        return;
    }

    if (c->outlen + len > c->outcap) {
        if (c->outoff > 0) {
            memmove(c->outbuf, c->outbuf + c->outoff, pending);
            c->outoff = 0;
            c->outlen = pending;
        }

        if (c->outlen + len > c->outcap) {
            size_t cap = c->outcap ? c->outcap : IPC_OUTBUF_SOFT;
            while (cap < c->outlen + len) cap *= 2;
            if (cap > IPC_OUTBUF_HARD) cap = IPC_OUTBUF_HARD;

            char *nb = realloc(c->outbuf, cap);
            if (!nb) {
                c->dead = true;
                return;
            }

            c->outbuf = nb;
            c->outcap = cap;
        }
    }

    memcpy(c->outbuf + c->outlen, data, len);
    c->outlen += len;
    client_sync_events(c);
}

static void client_destroy(struct IpcClient *c) {
    if (c->source != NULL) wl_event_source_remove(c->source);
    wl_list_remove(&c->link);
    ipc_client_count--;
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

static struct {
    char *data;
    size_t len, cap;
    bool failed;
} line;

static bool line_reserve(size_t extra) {
    if (line.failed) return false;
    if (line.len > IPC_LINE_MAX || extra > IPC_LINE_MAX - line.len) {
        line.failed = true;
        return false;
    }

    size_t need = line.len + extra;
    if (need <= line.cap) return true;

    size_t cap = line.cap ? line.cap : 256;
    while (cap < need) cap *= 2;

    char *p = realloc(line.data, cap);
    if (!p) {
        line.failed = true;
        return false;
    }

    line.data = p;
    line.cap = cap;
    return true;
}

static void line_put(const char *s, size_t n) {
    if (n == 0 || !line_reserve(n)) return;
    memcpy(line.data + line.len, s, n);
    line.len += n;
}

#define line_lit(s) line_put((s), sizeof(s) - 1)

__attribute__((format(printf, 1, 2))) static void
line_printf(const char *fmt, ...) {
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        line.failed = true;
        return;
    }

    line_put(tmp, (size_t)n);
}

static void line_quoted(const char *s) {
    if (!s) s = "";
    line_lit("\"");

    const char *run = s;
    for (const char *p = s;; p++) {
        const char *esc = NULL;
        switch (*p) {
            case '"': esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n"; break;
            case '\r': esc = "\\r"; break;
            case '\t': esc = "\\t"; break;
            case '\0': break;
            default: continue;
        }

        line_put(run, (size_t)(p - run));
        if (!esc) break;
        line_put(esc, 2);
        run = p + 1;
    }

    line_lit("\"");
}

static void line_send(struct IpcClient *to) {
    line_lit("\n");

    if (line.failed) {
        log_warn("Dropping invalid IPC line");
        if (to) to->dead = true;
    } else if (to) {
        outbuf_append(to, line.data, line.len);
    } else {
        struct IpcClient *c;
        wl_list_for_each(c, &ipc_clients, link) {
            if (c->subscribed) outbuf_append(c, line.data, line.len);
        }
    }

    line.len = 0;
    line.failed = false;
}

struct Value {
    enum { V_INT, V_FLOAT, V_BOOL, V_STR } type;
    union {
        long i;
        double f;
        bool b;
        const char *s;
    } u;
};

static struct Value v_int(long i) {
    return (struct Value){.type = V_INT, .u.i = i};
}

static struct Value v_float(float f) {
    return (struct Value){.type = V_FLOAT, .u.f = (double)f};
}

static struct Value v_bool(bool b) {
    return (struct Value){.type = V_BOOL, .u.b = b};
}

static struct Value v_str(const char *s) {
    return (struct Value){.type = V_STR, .u.s = s ? s : ""};
}

static void line_value(const struct Value *v) {
    switch (v->type) {
        case V_INT: line_printf("%ld", v->u.i); break;
        case V_FLOAT: line_printf("%g", v->u.f); break;
        case V_BOOL:
            if (v->u.b) {
                line_lit("true");
            } else {
                line_lit("false");
            }
            break;
        case V_STR: line_quoted(v->u.s); break;
    }
}

static void emit_kv(
    struct IpcClient *to, const char *tag, const char *name, struct Value v
) {
    line_printf("%s %s ", tag, name);
    line_value(&v);
    line_send(to);
}

static void respond(struct IpcClient *c, const char *err) {
    if (err) {
        line_printf("ERR %s", err);
    } else {
        line_lit("OK");
    }

    line_send(c);
}

static const char *reply_value(struct IpcClient *c, struct Value v) {
    line_lit("OK ");
    line_value(&v);
    line_send(c);
    return NULL;
}

static void act_switch_space(int s) { wm_switch_space(s); }

static void act_move_to_space(int s) {
    wm_move_window_to_space(window_focused(), s);
}

static void act_set_layout(int l) { wm_set_layout((enum Layout)l); }

static void act_cycle_layout(int unused) {
    (void)unused;
    wm_set_layout((enum Layout)((wm.layout + 1) % ((int)LAYOUT_LAST + 1)));
}

static void act_close(int unused) {
    (void)unused;
    struct Window *w = window_focused();
    if (w != NULL) window_close(w);
}

// Returns NULL/error text
static const char *queue_action(IpcAction action, int arg) {
    if (server.locked) return "SESSION LOCKED";
    if (ipc_pending_count >= IPC_PENDING_MAX) return "BUSY";

    struct IpcPending *p = calloc(1, sizeof(*p));
    if (p == NULL) return "OOM";

    p->action = action;
    p->arg = arg;
    wl_list_insert(ipc_pending.prev, &p->link);
    ipc_pending_count++;
    return NULL;
}

static const char *queue_space_action(IpcAction action, const char *arg) {
    int s;
    if (!parse_space(arg, &s)) return "bad space";
    return queue_action(action, s);
}

void ipc_process_pending_commands(void) {
    if (!ipc_initialized) return;

    struct IpcPending *p, *tmp;
    wl_list_for_each_safe(p, tmp, &ipc_pending, link) {
        wl_list_remove(&p->link);
        ipc_pending_count--;
        if (!server.locked) p->action(p->arg);
        free(p);
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

static int space_window_count(int space);

static const char *get_layout(const char *arg, struct Value *out) {
    (void)arg;
    *out = v_str(layout_name(wm.layout));
    return NULL;
}

static const char *get_current_space(const char *arg, struct Value *out) {
    (void)arg;
    *out = v_int(wm.current_space);
    return NULL;
}

static const char *get_space_window_count(const char *arg, struct Value *out) {
    int s;
    if (!parse_space(arg, &s)) return "bad space";
    *out = v_int(space_window_count(s));
    return NULL;
}

static const char *get_focused_title(const char *arg, struct Value *out) {
    (void)arg;
    struct Window *w = window_focused();
    *out = v_str(w ? w->title : NULL);
    return NULL;
}

static const char *get_focused_app_id(const char *arg, struct Value *out) {
    (void)arg;
    struct Window *w = window_focused();
    *out = v_str(w ? w->app_id : NULL);
    return NULL;
}

static const char *get_mfact(const char *arg, struct Value *out) {
    (void)arg;
    *out = v_float(wm.mfact);
    return NULL;
}

static const char *set_layout(const char *value) {
    enum Layout l;
    if (!parse_layout(value, &l)) return "bad layout";
    return queue_action(act_set_layout, (int)l);
}

static const char *set_current_space(const char *value) {
    return queue_space_action(act_switch_space, value);
}

static const char *set_mfact(const char *value) {
    float v;
    if (!parse_float(value, &v) || v <= 0.0f || v >= 1.0f) return "bad value";

    wm.mfact = v;
    emit_kv(NULL, "EVT", "mfact", v_float(v));
    return NULL;
}

#ifdef WALLPAPER
static const char *get_wallpaper(const char *arg, struct Value *out) {
    (void)arg;
    *out = v_str(wm.wallpaper.path);
    return NULL;
}

static const char *set_wallpaper(const char *value) {
    if (value[0] == '\0') return "bad value";
    if (!wm_set_wallpaper_path(value)) return "could not load";

    emit_kv(NULL, "EVT", "wallpaper_path", v_str(wm.wallpaper.path));
    return NULL;
}
#endif

static const struct SpecialOps {
    const char *name;
    bool dump;
    const char *(*get)(const char *arg, struct Value *out); // NULL: none
    const char *(*set)(const char *value);                  // NULL: none
} special_ops[] = {
    {            "layout",  true,             get_layout,        set_layout},
    {     "current_space",  true,      get_current_space, set_current_space},
    {"space_window_count", false, get_space_window_count,              NULL},
    {     "focused_title", false,      get_focused_title,              NULL},
    {    "focused_app_id", false,     get_focused_app_id,              NULL},
    {             "mfact",  true,              get_mfact,         set_mfact},
#ifdef WALLPAPER
    {    "wallpaper_path",  true,          get_wallpaper,     set_wallpaper},
#endif
};

static const struct SpecialOps *special_find(const char *name) {
    for (size_t i = 0; i < ARRAY_LEN(special_ops); i++) {
        if (!strcmp(special_ops[i].name, name)) return &special_ops[i];
    }
    return NULL;
}

typedef bool (*KeyVisitor)(const char *name, struct Value v, void *ud);

static bool special_visit(
    const char *name, unsigned access, bool all_keys, KeyVisitor visit, void *ud
) {
    const struct SpecialOps *op = special_find(name);
    struct Value v;

    if (op == NULL || !op->dump || op->get == NULL) return false;
    if (!all_keys && !(access & IPC_A_R)) return false;
    if (op->get(NULL, &v) != NULL) return false;
    return visit(name, v, ud);
}

static bool keys_visit(KeyVisitor visit, void *ud, bool all_keys) {
#define IPC_KEY_INT(name, access, after, desc, field, min, max)                \
    if ((all_keys || ((access) & IPC_A_R))                                     \
        && visit(name, v_int((long)(field)), ud))                              \
        return true;
#define IPC_KEY_FLOAT(name, access, after, desc, field, min, max)              \
    if ((all_keys || ((access) & IPC_A_R))                                     \
        && visit(name, v_float((float)(field)), ud))                           \
        return true;
#define IPC_KEY_BOOL(name, access, after, range, desc, field)                  \
    if ((all_keys || ((access) & IPC_A_R)) && visit(name, v_bool(field), ud))  \
        return true;
#define IPC_KEY_STR(name, access, after, range, desc, field)                   \
    if ((all_keys || ((access) & IPC_A_R)) && visit(name, v_str(field), ud))   \
        return true;
#define IPC_KEY_SPECIAL(name, access, range, desc)                             \
    if (special_visit(name, (access), all_keys, visit, ud)) return true;

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
    return false;
}

static const char *
special_get(struct IpcClient *c, const char *name, const char *arg) {
    const struct SpecialOps *op = special_find(name);
    struct Value v;

    if (op == NULL || op->get == NULL) return "unknown key";

    const char *err = op->get(arg, &v);
    if (err) return err;
    return reply_value(c, v);
}

static const char *
get_key(struct IpcClient *c, const char *key, const char *arg) {
#define IPC_KEY_INT(name, access, after, desc, field, min, max)                \
    if (((access) & IPC_A_R) && !strcmp(key, name))                            \
        return reply_value(c, v_int((long)(field)));
#define IPC_KEY_FLOAT(name, access, after, desc, field, min, max)              \
    if (((access) & IPC_A_R) && !strcmp(key, name))                            \
        return reply_value(c, v_float((float)(field)));
#define IPC_KEY_BOOL(name, access, after, range, desc, field)                  \
    if (((access) & IPC_A_R) && !strcmp(key, name))                            \
        return reply_value(c, v_bool(field));
#define IPC_KEY_STR(name, access, after, range, desc, field)                   \
    if (((access) & IPC_A_R) && !strcmp(key, name))                            \
        return reply_value(c, v_str(field));
#define IPC_KEY_SPECIAL(name, access, range, desc)                             \
    if (((access) & IPC_A_R) && !strcmp(key, name))                            \
        return special_get(c, name, arg);

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

    return "unknown key";
}

static const char *
special_set(const char *name, unsigned access, const char *value) {
    const struct SpecialOps *op = special_find(name);
    if (!(access & IPC_A_W) || op == NULL || op->set == NULL)
        return "unknown/read-only key";
    return op->set(value);
}

// Returns NULL/error text
static const char *set_key(const char *key, const char *value) {
#define IPC_KEY_INT(name, access, after, desc, field, min, max)                \
    if (((access) & IPC_A_W) && !strcmp(key, name)) {                          \
        long v;                                                                \
        if (!parse_int(value, &v) || v < (min) || v > (max))                   \
            return "bad value";                                                \
        (field) = (int32_t)v;                                                  \
        emit_kv(NULL, "EVT", name, v_int(v));                                  \
        after_set(after);                                                      \
        return NULL;                                                           \
    }
#define IPC_KEY_FLOAT(name, access, after, desc, field, min, max)              \
    if (((access) & IPC_A_W) && !strcmp(key, name)) {                          \
        float v;                                                               \
        if (!parse_float(value, &v) || v < (min) || v > (max))                 \
            return "bad value";                                                \
        (field) = v;                                                           \
        emit_kv(NULL, "EVT", name, v_float(v));                                \
        after_set(after);                                                      \
        return NULL;                                                           \
    }
#define IPC_KEY_BOOL(name, access, after, range, desc, field)                  \
    if (((access) & IPC_A_W) && !strcmp(key, name)) {                          \
        bool b;                                                                \
        if (!parse_bool(value, &b)) return "bad bool";                         \
        (field) = b;                                                           \
        emit_kv(NULL, "EVT", name, v_bool(b));                                 \
        after_set(after);                                                      \
        return NULL;                                                           \
    }
#define IPC_KEY_STR(name, access, after, range, desc, field) // read-only
#define IPC_KEY_SPECIAL(name, access, range, desc)                             \
    if (!strcmp(key, name)) return special_set(name, (access), value);

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

    return "unknown/read-only key";
}

static void check_special(const char *name, unsigned access) {
    const struct SpecialOps *op = special_find(name);

#ifndef WALLPAPER
    if (!strcmp(name, "wallpaper_path")) return;
#endif

    if (op == NULL) {
        log_warn("keys.h special key '%s' has no handler", name);
        return;
    }

    if (((access & IPC_A_R) != 0) != (op->get != NULL))
        log_warn("Special key '%s': read access and handler disagree", name);

    if (((access & IPC_A_W) != 0) != (op->set != NULL))
        log_warn("Special key '%s': write access and handler disagree", name);
}

static void verify_special_keys(void) {
#define IPC_KEY_INT(name, access, after, desc, field, min, max)
#define IPC_KEY_FLOAT(name, access, after, desc, field, min, max)
#define IPC_KEY_BOOL(name, access, after, range, desc, field)
#define IPC_KEY_STR(name, access, after, range, desc, field)
#define IPC_KEY_SPECIAL(name, access, range, desc)                             \
    check_special(name, (access));

    KEYS_TABLE(
        IPC_KEY_INT, IPC_KEY_FLOAT, IPC_KEY_BOOL, IPC_KEY_STR, IPC_KEY_SPECIAL
    )

#undef IPC_KEY_INT
#undef IPC_KEY_FLOAT
#undef IPC_KEY_BOOL
#undef IPC_KEY_STR
#undef IPC_KEY_SPECIAL
}

static uint64_t window_id(const struct Window *w) {
    return (uint64_t)(uintptr_t)w;
}

static int space_window_count(int space) {
    int n = 0;
    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (!w->closed && w->space == space) n++;
    }

    return n;
}

static void emit_space_counts(struct IpcClient *to) {
    for (int s = 0; s < SPACE_COUNT; s++) {
        line_printf("EVT space_window_count %d %d", s, space_window_count(s));
        line_send(to);
    }
}

static void emit_focus(struct IpcClient *to) {
    struct Window *w = window_focused();

    line_printf("EVT focused %llu", (unsigned long long)(w ? window_id(w) : 0));
    line_send(to);
    emit_kv(to, "EVT", "focused_title", v_str(w ? w->title : NULL));
    emit_kv(to, "EVT", "focused_app_id", v_str(w ? w->app_id : NULL));
}

static void emit_window(struct IpcClient *to, const struct Window *w) {
    line_printf(
        "EVT window %llu space %d title ", (unsigned long long)window_id(w),
        w->space
    );

    line_quoted(w->title);
    line_lit(" app_id ");
    line_quoted(w->app_id);
    line_send(to);
}

static void emit_window_event(const char *name, const struct Window *w) {
    line_printf("EVT %s %llu", name, (unsigned long long)window_id(w));
    line_send(NULL);
}

struct DumpCtx {
    const char *tag;
    struct IpcClient *to;
    bool all_keys;
};

static bool visit_dump(const char *name, struct Value v, void *ud) {
    const struct DumpCtx *d = ud;
    emit_kv(d->to, d->tag, name, v);
    return false;
}

static void emit_all_state(struct IpcClient *to) {
    struct DumpCtx d = {"EVT", to, true};
    keys_visit(visit_dump, &d, d.all_keys);
    emit_space_counts(to);
    emit_focus(to);

    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (!w->closed) emit_window(to, w);
    }

    line_lit("EVT ready");
    line_send(to);
}

void ipc_notify_layout(void) {
    if (!ipc_initialized) return;
    emit_kv(NULL, "EVT", "layout", v_str(layout_name(wm.layout)));
}

void ipc_notify_space(void) {
    if (!ipc_initialized) return;
    emit_kv(NULL, "EVT", "current_space", v_int(wm.current_space));
    emit_space_counts(NULL);
}

void ipc_notify_focus(void) {
    if (!ipc_initialized) return;
    emit_focus(NULL);
}

void ipc_notify_kb_layout(void) {
    if (!ipc_initialized) return;
    emit_kv(NULL, "EVT", "kb_layout", v_str(wm.kb_layout));
}

void ipc_notify_window_opened(struct Window *w) {
    if (!ipc_initialized || !w) return;
    emit_window_event("window_opened", w);
    emit_window(NULL, w);
    ipc_notify_space();
    ipc_notify_focus();
}

void ipc_notify_window_closed(struct Window *w) {
    if (!ipc_initialized || !w) return;
    emit_window_event("window_closed", w);
    ipc_notify_space();
    ipc_notify_focus();
}

void ipc_notify_window_meta(struct Window *w) {
    if (!ipc_initialized || !w) return;
    emit_window(NULL, w);
    if (w == window_focused()) ipc_notify_focus();
}

void ipc_notify_all_state(void) {
    if (!ipc_initialized) return;
    emit_all_state(NULL);
}

void ipc_flush_pending(void) {
    if (!ipc_initialized) return;

    struct IpcClient *c;
    wl_list_for_each(c, &ipc_clients, link) {
        if (!c->dead) client_flush(c);
    }

    reap_dead_clients();
}

static void cmd_ping(struct IpcClient *c, char *args) {
    (void)args;
    respond(c, NULL);
}

static void cmd_quit(struct IpcClient *c, char *args) {
    (void)args;
    c->dead = true;
}

static void cmd_config(struct IpcClient *c, char *args) {
    (void)args;
    struct DumpCtx d = {"CFG", c, false};
    keys_visit(visit_dump, &d, d.all_keys);
    respond(c, NULL);
}

static void cmd_subscribe(struct IpcClient *c, char *args) {
    (void)args;
    respond(c, NULL);
    emit_all_state(c);
    c->subscribed = true;
}

static void cmd_unsubscribe(struct IpcClient *c, char *args) {
    (void)args;
    c->subscribed = false;
    respond(c, NULL);
}

static void cmd_get(struct IpcClient *c, char *args) {
    if (!args) {
        respond(c, "key is missing");
        return;
    }

    const char *key = args;
    const char *arg = split_token(args);

    const char *err = get_key(c, key, arg);
    if (err) respond(c, err);
}

static void cmd_set(struct IpcClient *c, char *args) {
    if (!args) {
        respond(c, "key is missing");
        return;
    }

    char *value = split_token(args);
    if (!value) {
        respond(c, "value is missing");
        return;
    }

    respond(c, set_key(args, value));
}

static void cmd_dpms(struct IpcClient *c, char *args) {
    static const struct {
        const char *name;
        int mode;
    } modes[] = {
        {   "off", 0},
        {    "on", 1},
        {"toggle", 2}
    };

    if (!args) {
        respond(c, "mode is missing");
        return;
    }

    char *output = split_token(args);

    int mode = -1;
    for (size_t i = 0; i < ARRAY_LEN(modes); i++) {
        if (!strcmp(args, modes[i].name)) mode = modes[i].mode;
    }

    if (mode < 0) {
        respond(c, "bad mode (on|off|toggle)");
        return;
    }

    if (output_set_power(output, mode) == 0) {
        respond(c, output ? "no output" : "no outputs");
        return;
    }

    respond(c, NULL);
}

static const struct Verb {
    const char *name;
    void (*handler)(struct IpcClient *, char *args);
} verbs[] = {
    {       "PING",        cmd_ping},
    {       "QUIT",        cmd_quit},
    {     "CONFIG",      cmd_config},
    {  "SUBSCRIBE",   cmd_subscribe},
    {"UNSUBSCRIBE", cmd_unsubscribe},
    {        "GET",         cmd_get},
    {        "SET",         cmd_set},
    {       "DPMS",        cmd_dpms},
};

static const struct QueuedVerb {
    const char *name;
    IpcAction action;
    bool takes_space;
} queued_verbs[] = {
    { "SWITCH_SPACE",  act_switch_space,  true},
    {"MOVE_TO_SPACE", act_move_to_space,  true},
    { "CYCLE_LAYOUT",  act_cycle_layout, false},
    {        "CLOSE",         act_close, false},
};

static void dispatch_line(struct IpcClient *c, char *text) {
    text = skip_ws(text);

    size_t len = strlen(text);
    while (len > 0
           && (text[len - 1] == '\r' || text[len - 1] == ' '
               || text[len - 1] == '\t')) {
        text[--len] = '\0';
    }

    if (len == 0) return;

    char *args = split_token(text);

    for (size_t i = 0; i < ARRAY_LEN(verbs); i++) {
        if (!strcmp(text, verbs[i].name)) {
            verbs[i].handler(c, args);
            return;
        }
    }

    for (size_t i = 0; i < ARRAY_LEN(queued_verbs); i++) {
        const struct QueuedVerb *v = &queued_verbs[i];
        if (strcmp(text, v->name) != 0) continue;
        respond(
            c, v->takes_space ? queue_space_action(v->action, args)
                              : queue_action(v->action, 0)
        );
        return;
    }

    respond(c, "unknown command");
}

static void client_read(struct IpcClient *c) {
    if (c->dead || c->read_closed) return;

    ssize_t n;
    do {
        n = recv(c->fd, c->inbuf + c->inlen, sizeof(c->inbuf) - c->inlen, 0);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) c->dead = true;
        return;
    }

    if (n == 0) {
        c->read_closed = true;
        return;
    }

    c->inlen += (size_t)n;

    size_t start = 0;
    char *nl;
    while (!c->dead && (nl = memchr(c->inbuf + start, '\n', c->inlen - start))
    ) {
        *nl = '\0';
        dispatch_line(c, c->inbuf + start);
        start = (size_t)(nl - c->inbuf) + 1;
    }

    if (c->dead) return;

    if (start > 0) {
        memmove(c->inbuf, c->inbuf + start, c->inlen - start);
        c->inlen -= start;
    }

    if (c->inlen == sizeof(c->inbuf)) c->dead = true; // line too long
}

static int client_event_handler(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct IpcClient *c = data;

    if (mask & WL_EVENT_ERROR) c->dead = true;
    if (!c->dead && (mask & (WL_EVENT_READABLE | WL_EVENT_HANGUP)))
        client_read(c);

    if (!c->dead) client_flush(c);
    if (c->dead) reap_dead_clients();

    return 0;
}

static void reject_client(int fd, const char *msg) {
    if (set_nosigpipe(fd)) (void)send(fd, msg, strlen(msg), 0);
    close(fd);
}

static void accept_clients(void) {
    reap_dead_clients();

    for (;;) {
        int fd =
            accept4(ipc_server_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);

        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;

            if ((errno == EMFILE || errno == ENFILE) && ipc_reserve_fd >= 0) {
                log_warn("Out of file descriptors, terminating");

                close(ipc_reserve_fd);
                int shed = accept4(ipc_server_fd, NULL, NULL, SOCK_CLOEXEC);
                if (shed >= 0) reject_client(shed, "ERR too many clients\n");
                ipc_reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
                return;
            }

            log_warn("accept() failed: %s", strerror(errno));
            return;
        }

        if (ipc_client_count >= IPC_MAX_CLIENTS) {
            reject_client(fd, "ERR too many clients\n");
            continue;
        }

        if (!set_nosigpipe(fd)) {
            log_warn("setsockopt(SO_NOSIGPIPE) failed: %s", strerror(errno));
            close(fd);
            continue;
        }

        struct IpcClient *c = calloc(1, sizeof(*c));
        if (!c) {
            close(fd);
            return;
        }

        c->fd = fd;
        c->mask = WL_EVENT_READABLE;
        wl_list_insert(ipc_clients.prev, &c->link);
        ipc_client_count++;

        c->source = wl_event_loop_add_fd(
            ipc_loop, fd, WL_EVENT_READABLE, client_event_handler, c
        );

        if (c->source == NULL) client_destroy(c);
    }
}

static int server_event_handler(int fd, uint32_t mask, void *data) {
    (void)fd;
    (void)data;

    if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
        log_warn("Server socket failed; no new connections will be accepted");

        wl_event_source_remove(ipc_server_source);
        ipc_server_source = NULL;
        return 0;
    }

    if (mask & WL_EVENT_READABLE) accept_clients();
    return 0;
}

static int socket_fail(int fd, bool unlink_path) {
    if (unlink_path) unlink(ipc_path);
    ipc_path[0] = '\0';
    close(fd);
    return -1;
}

static bool socket_in_use(const struct sockaddr_un *addr, socklen_t len) {
    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe < 0) return false;

    bool live = connect(probe, (const struct sockaddr *)addr, len) == 0;
    close(probe);
    return live;
}

static int make_socket(void) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) {
        log_warn("XDG_RUNTIME_DIR is not set");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    int n = snprintf(
        addr.sun_path, sizeof(addr.sun_path), "%s/nullspace-%u.sock", runtime,
        (unsigned)getpid()
    );

    if (n < 0 || (size_t)n >= sizeof(addr.sun_path)) {
        log_warn("Socket path too long (limit %zu)", sizeof(addr.sun_path) - 1);
        return -1;
    }

    const socklen_t addrlen = (socklen_t)SUN_LEN(&addr);

    if (socket_in_use(&addr, addrlen)) {
        log_warn("Another instance is already listening on %s", addr.sun_path);
        return -1;
    }

    snprintf(ipc_path, sizeof(ipc_path), "%s", addr.sun_path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        log_warn("socket() failed: %s", strerror(errno));
        ipc_path[0] = '\0';
        return -1;
    }

    if (!set_nosigpipe(fd)) {
        log_warn("setsockopt(SO_NOSIGPIPE) failed: %s", strerror(errno));
        return socket_fail(fd, false);
    }

    unlink(ipc_path);

    mode_t old_umask = umask(0177);
    int rc = bind(fd, (struct sockaddr *)&addr, addrlen);
    int bind_errno = errno;
    umask(old_umask);

    if (rc < 0) {
        log_warn("bind(%s) failed: %s", ipc_path, strerror(bind_errno));
        return socket_fail(fd, false);
    }

    if (listen(fd, SOMAXCONN) < 0) {
        log_warn("listen() failed: %s", strerror(errno));
        return socket_fail(fd, true);
    }

    ipc_reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (ipc_reserve_fd < 0) {
        log_warn("Reserve open(/dev/null) failed: %s", strerror(errno));
    }

    setenv("NULLSPACE_INSTANCE_SIGNATURE", ipc_path, 1);
    return fd;
}

static void register_server_source(void) {
    if (ipc_loop == NULL || ipc_server_fd < 0 || ipc_server_source != NULL)
        return;

    ipc_server_source = wl_event_loop_add_fd(
        ipc_loop, ipc_server_fd, WL_EVENT_READABLE, server_event_handler, NULL
    );

    if (ipc_server_source == NULL)
        log_warn("Failed to register IPC socket with the event loop");
}

void ipc_attach_event_loop(struct wl_event_loop *loop) {
    ipc_loop = loop;
    register_server_source();
}

void ipc_init(void) {
    if (ipc_initialized) return;

    verify_special_keys();

    ipc_server_fd = make_socket();
    if (ipc_server_fd < 0) {
        log_warn("Socket unavailable");
        return;
    }

    ipc_initialized = true;
    log_info("Listening on '%s'", ipc_path);
    register_server_source();
}

void ipc_destroy(void) {
    if (!ipc_initialized) return;

    unsetenv("NULLSPACE_INSTANCE_SIGNATURE");

    struct IpcClient *c, *ctmp;
    wl_list_for_each_safe(c, ctmp, &ipc_clients, link) { client_destroy(c); }

    struct IpcPending *p, *ptmp;
    wl_list_for_each_safe(p, ptmp, &ipc_pending, link) {
        wl_list_remove(&p->link);
        free(p);
    }
    ipc_pending_count = 0;

    if (ipc_server_source != NULL) {
        wl_event_source_remove(ipc_server_source);
        ipc_server_source = NULL;
    }

    if (ipc_reserve_fd >= 0) close(ipc_reserve_fd);
    ipc_reserve_fd = -1;

    if (ipc_server_fd >= 0) close(ipc_server_fd);
    ipc_server_fd = -1;

    if (ipc_path[0]) unlink(ipc_path);
    ipc_path[0] = '\0';

    free(line.data);
    memset(&line, 0, sizeof(line));

    ipc_initialized = false;
    ipc_loop = NULL;
}
