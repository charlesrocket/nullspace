#include <err.h>
#include <errno.h>
#include <ns-util.h>
#include <nullspace-controller-v1-client-protocol.h>
#include <nullspace/keys.h>
#include <nullspace/layouts.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/signal.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-util.h>
#include <wlr-output-power-management-unstable-v1-client-protocol.h>
#include <xkbcommon/xkbcommon.h>

struct Nctl {
    struct wl_display* display;
    struct nullspace_controller_v1* controller;

    FILE* out;
    char* out_buf;
    size_t out_len;

    uint32_t next_serial, // next serial to use for our own requests
        our_serial;       // serial of the request currently in flight

    bool print_key, got_done, got_error;
};

struct Cmd {
    const char* usage;
    const char* desc;

    int min_args, max_args;
    bool offline;
    bool (*run)(struct Nctl* n, char* const* args);
};

struct LayoutEntry {
    const char* name;
    const char* desc;
};

#define DPMS_MAX_OUTPUTS 32

enum DpmsMode { DPMS_OFF, DPMS_ON, DPMS_TOGGLE };

struct DpmsOut {
    struct wl_output* wl_output;
    struct zwlr_output_power_v1* power;

    char* name;
    uint32_t mode;
    bool mode_known, failed;
};

struct DpmsCtx {
    struct wl_display* display;
    struct wl_registry* registry;
    struct zwlr_output_power_manager_v1* manager;
    struct DpmsOut outputs[DPMS_MAX_OUTPUTS];
    uint32_t n_outputs;
};

static const struct LayoutEntry layout_table[] = {
#define LAYOUT_ENTRY(id, label, desc) {(label), (desc)},
    LAYOUTS_TABLE(LAYOUT_ENTRY)
#undef LAYOUT_ENTRY
};

#define RANGE_STR_INNER_(x) #x
#define RANGE_STR_(x)       RANGE_STR_INNER_(x)
#define RANGE_STR(min, max) RANGE_STR_(min) "," RANGE_STR_(max)

static const struct IpcKeyMeta nctl_keys[] = {
#define IPC_KEY_INT(name, access, after, desc, field, min, max)                \
    {name, IPC_TYPE_INT, (access), RANGE_STR(min, max), (desc)},
#define IPC_KEY_FLOAT(name, access, after, desc, field, min, max)              \
    {name, IPC_TYPE_FLOAT, (access), RANGE_STR(min, max), (desc)},
#define IPC_KEY_BOOL(name, access, after, range, desc, field)                  \
    {name, IPC_TYPE_BOOL, (access), (range), (desc)},
#define IPC_KEY_STR(name, access, after, range, desc, field)                   \
    {name, IPC_TYPE_STR, (access), (range), (desc)},
#define IPC_KEY_SPECIAL(name, access, range, desc)                             \
    {name, IPC_TYPE_SPECIAL, (access), (range), (desc)},

    KEYS_TABLE(
        IPC_KEY_INT, IPC_KEY_FLOAT, IPC_KEY_BOOL, IPC_KEY_STR, IPC_KEY_SPECIAL
    )
        KEYS_TABLE_WALLPAPER(
            IPC_KEY_INT, IPC_KEY_FLOAT, IPC_KEY_BOOL, IPC_KEY_STR,
            IPC_KEY_SPECIAL
        )
#undef IPC_KEY_INT
#undef IPC_KEY_FLOAT
#undef IPC_KEY_BOOL
#undef IPC_KEY_STR
#undef IPC_KEY_SPECIAL
};

struct Style {
    const char *bold, *head, *reset;
};

static struct Style style_for(int fd) {
    const char* nc = getenv("NO_COLOR");
    bool on = isatty(fd) && !(nc != NULL && *nc != '\0');
    if (on) return (struct Style){"\033[1m", "\033[1m\033[4m", "\033[0m"};
    return (struct Style){"", "", ""};
}

static void put_col(const char* s, size_t w) {
    size_t n = strlen(s);
    fputs(s, stdout);
    for (; n < w; n++) putchar(' ');
    putchar(' ');
}

static void put_heading(const struct Style* st, const char* title) {
    fputs(st->head, stdout);
    fputs(title, stdout);
    fputs(st->reset, stdout);
    fputs("\n\n", stdout);
}

static FILE* reply_stream(struct Nctl* n) {
    if (n->out != NULL) return n->out;

    n->out = open_memstream(&n->out_buf, &n->out_len);
    if (n->out == NULL) {
        n->out = stdout;
        n->out_buf = NULL;
        n->out_len = 0;
    }

    return n->out;
}

static void reply_finish(struct Nctl* n, bool flush) {
    if (n->out == NULL) return;
    if (n->out != stdout) {
        if (fflush(n->out) != 0 || ferror(n->out)) {
            warnx("reply buffer flush failed");
        }

        if (fclose(n->out) != 0) warnx("reply buffer close failed");

        if (flush && n->out_len > 0) fwrite(n->out_buf, 1, n->out_len, stdout);

        free(n->out_buf);
    }

    n->out = NULL;
    n->out_buf = NULL;
    n->out_len = 0;
}

static void ctl_config_value(
    void* data, struct nullspace_controller_v1* c, const char* key,
    const char* value
) {
    (void)c;

    struct Nctl* n = data;
    FILE* f = reply_stream(n);
    if (n->print_key)
        fprintf(f, "%s = %s\n", key, value);
    else
        fprintf(f, "%s\n", value);
}

static void ctl_config_key(
    void* data, struct nullspace_controller_v1* c, const char* key,
    uint32_t type, uint32_t access, const char* range, const char* desc
) {
    (void)c;

    struct Nctl* n = data;
    FILE* f = reply_stream(n);

    const char* tn = "?";
    switch (type) {
        case NULLSPACE_CONTROLLER_V1_KEY_TYPE_INT: tn = "int"; break;
        case NULLSPACE_CONTROLLER_V1_KEY_TYPE_FLOAT: tn = "float"; break;
        case NULLSPACE_CONTROLLER_V1_KEY_TYPE_BOOL: tn = "bool"; break;
        case NULLSPACE_CONTROLLER_V1_KEY_TYPE_STRING: tn = "string"; break;
        case NULLSPACE_CONTROLLER_V1_KEY_TYPE_SPECIAL: tn = "special"; break;
    }

    const char* an = "?";
    switch (access) {
        case NULLSPACE_CONTROLLER_V1_ACCESS_READ: an = "read"; break;
        case NULLSPACE_CONTROLLER_V1_ACCESS_WRITE: an = "write"; break;
        case NULLSPACE_CONTROLLER_V1_ACCESS_READ_WRITE:
            an = "read_write";
            break;
    }

    fputs("  ", f);
    fputs(key, f);
    for (size_t i = strlen(key); i < 28; i++) fputc(' ', f);
    fputc(' ', f);
    fputs(tn, f);
    for (size_t i = strlen(tn); i < 8; i++) fputc(' ', f);
    fputc(' ', f);
    fputs(an, f);
    for (size_t i = strlen(an); i < 11; i++) fputc(' ', f);
    fputc(' ', f);

    if (range && *range) fprintf(f, "[%s] ", range);
    fprintf(f, "%s\n", desc ? desc : "");
}

static void ctl_space_info(
    void* data, struct nullspace_controller_v1* c, uint32_t index,
    uint32_t count, uint32_t urgent, uint32_t active
) {
    (void)c;
    struct Nctl* n = data;
    fprintf(
        reply_stream(n), "  space %u: windows=%u urgent=%s active=%s\n", index,
        count, urgent ? "yes" : "no", active ? "yes" : "no"
    );
}

static void ctl_layout_info(
    void* data, struct nullspace_controller_v1* c, const char* name,
    const char* desc, uint32_t active
) {
    (void)c;

    struct Nctl* n = data;
    FILE* f = reply_stream(n);
    fputs("  ", f);
    fputs(name, f);
    for (size_t i = strlen(name); i < 10; i++) fputc(' ', f);
    fprintf(f, "%s%s\n", desc ? desc : "", active ? "  [active]" : "");
}

static void
ctl_window_removed(void* data, struct nullspace_controller_v1* c, uint32_t id) {
    (void)c;

    struct Nctl* n = data;
    fprintf(reply_stream(n), "  removed: %u\n", id);
}

static void ctl_device_info(
    void* data, struct nullspace_controller_v1* c, const char* type,
    const char* name
) {
    (void)c;

    struct Nctl* n = data;
    fprintf(
        reply_stream(n), "  DEVICE %-12s name \"%s\"\n", type ? type : "?",
        name ? name : ""
    );
}

static void ctl_window_detail(
    void* data, struct nullspace_controller_v1* c, uint32_t id, int32_t pid,
    const char* ftl_id, const char* app_id, const char* title,
    const char* monitor, int32_t space, int32_t x, int32_t y, int32_t w,
    int32_t h, uint32_t state, const char* content_type, uint32_t effect_state
) {
    (void)c;
    struct Nctl* n = data;
    FILE* f = reply_stream(n);

    fprintf(f, "CLIENT %u pid %d", id, pid);
    if (ftl_id && *ftl_id) fprintf(f, " ftl \"%s\"", ftl_id);

    fprintf(
        f, " appid \"%s\" title \"%s\" monitor \"%s\"\n", app_id ? app_id : "",
        title ? title : "", monitor ? monitor : ""
    );

    fprintf(f, "  space %d x %d y %d width %d height %d\n", space, x, y, w, h);

    fputs("  state:", f);
    if (state & NULLSPACE_CONTROLLER_V1_WINDOW_STATE_MAPPED)
        fputs(" mapped", f);
    if (state & NULLSPACE_CONTROLLER_V1_WINDOW_STATE_FOCUSED)
        fputs(" focused", f);
    if (state & NULLSPACE_CONTROLLER_V1_WINDOW_STATE_FULLSCREEN)
        fputs(" fullscreen", f);
    if (state & NULLSPACE_CONTROLLER_V1_WINDOW_STATE_MAXIMIZED)
        fputs(" maximized", f);
    if (state & NULLSPACE_CONTROLLER_V1_WINDOW_STATE_MINIMIZED)
        fputs(" minimized", f);
    if (state & NULLSPACE_CONTROLLER_V1_WINDOW_STATE_FLOATING)
        fputs(" floating", f);
    if (state & NULLSPACE_CONTROLLER_V1_WINDOW_STATE_XWAYLAND)
        fputs(" xwayland", f);

    fputc('\n', f);
    fprintf(f, "  content_type %s\n", content_type ? content_type : "none");

    fputs("  effects:", f);
    if (effect_state & NULLSPACE_CONTROLLER_V1_WINDOW_FX_STATE_BLUR_RULE)
        fputs(" blur_rule", f);
    if (effect_state & NULLSPACE_CONTROLLER_V1_WINDOW_FX_STATE_EXT_SURFACE)
        fputs(" ext_surface", f);
    if (effect_state & NULLSPACE_CONTROLLER_V1_WINDOW_FX_STATE_EXT_BLUR)
        fputs(" ext_blur", f);
    if (effect_state & NULLSPACE_CONTROLLER_V1_WINDOW_FX_STATE_BLUR)
        fputs(" blur", f);
    if (effect_state & NULLSPACE_CONTROLLER_V1_WINDOW_FX_STATE_SHADOW)
        fputs(" shadow", f);

    fputc('\n', f);
}

static void ctl_display_info(
    void* data, struct nullspace_controller_v1* c, const char* name,
    const char* make, const char* model, const char* serial, int32_t x,
    int32_t y, int32_t w, int32_t h, int32_t pw, int32_t ph, const char* scale,
    int32_t refresh, uint32_t enabled, int32_t space, int32_t ax, int32_t ay,
    int32_t aw, int32_t ah
) {
    (void)c;

    struct Nctl* n = data;
    FILE* f = reply_stream(n);

    fprintf(
        f, "DISPLAY \"%s\" make \"%s\" model \"%s\" serial \"%s\"\n",
        name ? name : "", make ? make : "", model ? model : "",
        serial ? serial : ""
    );

    fprintf(f, "  x %d y %d width %d height %d\n", x, y, w, h);
    fprintf(
        f, "  phys_width %d phys_height %d scale %s refresh %d\n", pw, ph,
        scale ? scale : "1", refresh
    );

    fprintf(f, "  enabled %u space %d\n", enabled, space);
    fprintf(
        f, "  area_x %d area_y %d area_width %d area_height %d\n", ax, ay, aw,
        ah
    );
}

static void ctl_layer_info(
    void* data, struct nullspace_controller_v1* c, const char* ns,
    const char* layer, const char* output, uint32_t mapped, int32_t x,
    int32_t y, uint32_t w, uint32_t h, int32_t exclusive, const char* keyboard
) {
    (void)c;

    struct Nctl* n = data;
    FILE* f = reply_stream(n);

    fprintf(
        f, "LAYER \"%s\" layer %s output \"%s\" mapped %u\n", ns ? ns : "",
        layer ? layer : "?", output ? output : "", mapped
    );

    fprintf(f, "  x %d y %d width %u height %u\n", x, y, w, h);
    fprintf(
        f, "  exclusive_zone %d keyboard %s\n", exclusive,
        keyboard ? keyboard : "?"
    );
}

static const char* status_name(uint32_t code) {
    switch (code) {
        case NULLSPACE_CONTROLLER_V1_STATUS_UNKNOWN_KEY: return "unknown_key";
        case NULLSPACE_CONTROLLER_V1_STATUS_INVALID_VALUE:
            return "invalid_value";
        case NULLSPACE_CONTROLLER_V1_STATUS_READ_ONLY: return "read_only";
        case NULLSPACE_CONTROLLER_V1_STATUS_NOT_SUPPORTED:
            return "not_supported";
        case NULLSPACE_CONTROLLER_V1_STATUS_LOCKED: return "locked";
        case NULLSPACE_CONTROLLER_V1_STATUS_NO_SUCH_WINDOW:
            return "no_such_window";
        case NULLSPACE_CONTROLLER_V1_STATUS_NO_FOCUS: return "no_focus";
    }

    return "(unknown)";
}

static void ctl_error(
    void* data, struct nullspace_controller_v1* c, uint32_t serial,
    uint32_t code, const char* message
) {
    (void)c;

    struct Nctl* n = data;

    if (n->our_serial == 0 || serial != n->our_serial) {
        warnx("stray error for serial %u: %s", serial, status_name(code));
        return;
    }

    reply_finish(n, false);
    fprintf(
        stderr, "nctl: %s: %s\n", status_name(code),
        message ? message : "(no detail)"
    );

    n->got_error = true;
}

static void
ctl_done(void* data, struct nullspace_controller_v1* c, uint32_t serial) {
    (void)c;

    struct Nctl* n = data;

    if (n->our_serial != 0 && serial == n->our_serial) {
        reply_finish(n, true);
        n->got_done = true;
    } else if (serial == 0) {
        reply_finish(n, false); // unsolicited broadcast (discard)
    } else {
        warnx("done with unexpected serial %u", serial);
        reply_finish(n, false);
    }
}

static const struct nullspace_controller_v1_listener ctl_listener = {
    .config_value = ctl_config_value,
    .config_key = ctl_config_key,
    .space_info = ctl_space_info,
    .layout_info = ctl_layout_info,
    .window_removed = ctl_window_removed,
    .error = ctl_error,
    .done = ctl_done,
    .window_detail = ctl_window_detail,
    .device_info = ctl_device_info,
    .display_info = ctl_display_info,
    .layer_info = ctl_layer_info,
};

static void display_failed(struct wl_display* d) {
    const int e = wl_display_get_error(d);
    if (e == EPROTO) {
        const struct wl_interface* iface = NULL;
        uint32_t id = 0;
        const uint32_t code = wl_display_get_protocol_error(d, &iface, &id);
        warnx(
            "protocol error: code %u on %s (object %u)", code,
            iface ? iface->name : "(unknown)", id
        );
    } else if (e != 0) {
        warnx("compositor connection lost: %s", strerror(e));
    } else {
        warnx("compositor connection lost");
    }
}

static bool sync_display(struct wl_display* d) {
    if (wl_display_roundtrip(d) >= 0) return true;
    display_failed(d);
    return false;
}

static struct wl_display* connect_display(void) {
    struct wl_display* d = wl_display_connect(NULL);
    if (d == NULL)
        warnx("cannot connect to Wayland display (is WAYLAND_DISPLAY set?)");

    return d;
}

static void reg_global(
    void* data, struct wl_registry* reg, uint32_t name, const char* iface,
    uint32_t version
) {
    (void)version; // TODO

    struct Nctl* n = data;
    if (n->controller != NULL
        || strcmp(iface, nullspace_controller_v1_interface.name) != 0)
        return;

    n->controller =
        wl_registry_bind(reg, name, &nullspace_controller_v1_interface, 1);
    if (n->controller == NULL) {
        warnx("failed to bind nullspace_controller_v1");
        return;
    }

    nullspace_controller_v1_add_listener(n->controller, &ctl_listener, n);
}

static void
reg_global_remove(void* data, struct wl_registry* reg, uint32_t name) {
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = reg_global,
    .global_remove = reg_global_remove,
};

static bool ctl_connect(struct Nctl* n) {
    n->display = connect_display();
    if (n->display == NULL) return false;

    struct wl_registry* reg = wl_display_get_registry(n->display);
    if (reg == NULL) {
        warnx("could not get wl_registry");
        return false;
    }

    wl_registry_add_listener(reg, &registry_listener, n);
    const bool ok = sync_display(n->display);
    wl_registry_destroy(reg);
    if (!ok) return false;

    if (n->controller == NULL) {
        warnx("compositor does not advertise nullspace_controller_v1");
        return false;
    }

    return true;
}

static bool parse_uint(const char* what, const char* s, uint32_t* out) {
    long v;
    if (ns_parse_int(s, &v) && v >= 0 && (unsigned long)v <= UINT32_MAX) {
        *out = (uint32_t)v;
        return true;
    }

    warnx("bad %s '%s'", what, s);
    return false;
}

static void dpms_wl_geometry(
    void* data, struct wl_output* o, int32_t x, int32_t y, int32_t pw,
    int32_t ph, int32_t sub, const char* make, const char* model,
    int32_t transform
) {
    (void)data;
    (void)x;
    (void)y;
    (void)pw;
    (void)o;
    (void)ph;
    (void)sub;
    (void)model;
    (void)transform;
    (void)make;
}
static void dpms_wl_mode(
    void* data, struct wl_output* o, uint32_t flags, int32_t w, int32_t h,
    int32_t r
) {
    (void)data;
    (void)flags;
    (void)w;
    (void)h;
    (void)r;
    (void)o;
}
static void dpms_wl_done(void* data, struct wl_output* o) {
    (void)data;
    (void)o;
}
static void dpms_wl_scale(void* data, struct wl_output* o, int32_t s) {
    (void)data;
    (void)o;
    (void)s;
}
static void dpms_wl_name(void* data, struct wl_output* o, const char* name) {
    (void)o;

    struct DpmsOut* out = data;
    if (name == NULL) return;

    char* dup = strdup(name);
    if (dup == NULL) return;

    free(out->name);
    out->name = dup;
}

static void
dpms_wl_description(void* data, struct wl_output* o, const char* desc) {
    (void)data;
    (void)desc;
    (void)o;
}

static const struct wl_output_listener dpms_wl_listener = {
    .geometry = dpms_wl_geometry,
    .mode = dpms_wl_mode,
    .done = dpms_wl_done,
    .scale = dpms_wl_scale,
    .name = dpms_wl_name,
    .description = dpms_wl_description,
};

static void
dpms_power_mode(void* data, struct zwlr_output_power_v1* p, uint32_t mode) {
    (void)p;

    struct DpmsOut* out = data;
    out->mode = mode;
    out->mode_known = true;
}

static void dpms_power_failed(void* data, struct zwlr_output_power_v1* p) {
    (void)p;

    struct DpmsOut* out = data;
    out->failed = true;
}

static const struct zwlr_output_power_v1_listener dpms_power_listener = {
    .mode = dpms_power_mode,
    .failed = dpms_power_failed,
};

static void dpms_reg_global(
    void* data, struct wl_registry* reg, uint32_t name, const char* iface,
    uint32_t version
) {
    struct DpmsCtx* ctx = data;

    if (strcmp(iface, zwlr_output_power_manager_v1_interface.name) == 0) {
        if (ctx->manager != NULL) return;
        ctx->manager = wl_registry_bind(
            reg, name, &zwlr_output_power_manager_v1_interface, 1
        );

        return;
    }

    if (strcmp(iface, wl_output_interface.name) == 0) {
        if (ctx->n_outputs >= DPMS_MAX_OUTPUTS) {
            warnx("more than %d outputs, ignoring the rest", DPMS_MAX_OUTPUTS);
            return;
        }

        struct DpmsOut* out = &ctx->outputs[ctx->n_outputs];
        out->wl_output =
            wl_registry_bind(reg, name, &wl_output_interface, MIN(version, 4u));

        if (out->wl_output == NULL) return;

        wl_output_add_listener(out->wl_output, &dpms_wl_listener, out);
        ctx->n_outputs++;
    }
}

static void
dpms_reg_remove(void* data, struct wl_registry* reg, uint32_t name) {
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener dpms_registry_listener = {
    .global = dpms_reg_global,
    .global_remove = dpms_reg_remove,
};

static void dpms_ctx_cleanup(struct DpmsCtx* ctx) {
    for (uint32_t i = 0; i < ctx->n_outputs; i++) {
        struct DpmsOut* out = &ctx->outputs[i];
        if (out->power != NULL) zwlr_output_power_v1_destroy(out->power);
        if (out->wl_output != NULL) wl_output_destroy(out->wl_output);

        free(out->name);
    }

    if (ctx->manager != NULL)
        zwlr_output_power_manager_v1_destroy(ctx->manager);

    if (ctx->registry != NULL) wl_registry_destroy(ctx->registry);
    if (ctx->display != NULL) wl_display_disconnect(ctx->display);
}

static bool
dpms_run(struct DpmsCtx* ctx, const char* target, enum DpmsMode want) {
    if (!sync_display(ctx->display)) return false;

    if (ctx->manager == NULL) {
        warnx("compositor does not advertise zwlr_output_power_manager_v1");
        return false;
    }

    if (ctx->n_outputs == 0) {
        warnx("compositor has no outputs");
        return false;
    }

    for (uint32_t i = 0; i < ctx->n_outputs; i++) {
        struct DpmsOut* out = &ctx->outputs[i];
        out->power = zwlr_output_power_manager_v1_get_output_power(
            ctx->manager, out->wl_output
        );

        if (out->power == NULL) {
            warnx("could not create power object for output %u", i);
            return false;
        }

        zwlr_output_power_v1_add_listener(
            out->power, &dpms_power_listener, out
        );
    }

    if (!sync_display(ctx->display)) return false;

    bool any_hit = false, ok = true;
    for (uint32_t i = 0; i < ctx->n_outputs; i++) {
        struct DpmsOut* out = &ctx->outputs[i];
        const char* label = out->name ? out->name : "(unnamed)";

        if (target != NULL) {
            long idx;
            const bool matches =
                (out->name != NULL && strcmp(out->name, target) == 0)
                || (ns_parse_int(target, &idx) && idx == (long)i);

            if (!matches) continue;
            any_hit = true;
        }

        if (out->failed) {
            warnx(
                "output '%s': power control unavailable (unsupported/held by "
                "another client/gone)",
                label
            );

            ok = false;
            continue;
        }

        uint32_t m;
        if (want == DPMS_TOGGLE) {
            if (!out->mode_known) {
                warnx("output '%s': cannot toggle, state unknown", label);
                ok = false;
                continue;
            }

            m = (out->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON)
                  ? ZWLR_OUTPUT_POWER_V1_MODE_OFF
                  : ZWLR_OUTPUT_POWER_V1_MODE_ON;
        } else {
            m = (want == DPMS_ON) ? ZWLR_OUTPUT_POWER_V1_MODE_ON
                                  : ZWLR_OUTPUT_POWER_V1_MODE_OFF;
        }

        zwlr_output_power_v1_set_mode(out->power, m);
    }

    if (target != NULL && !any_hit) {
        warnx("no such output '%s'", target);
        return false;
    }

    return sync_display(ctx->display) && ok;
}

static bool cmd_dpms(struct Nctl* n, char* const* a) {
    (void)n;

    enum DpmsMode want;
    if (strcmp(a[0], "on") == 0)
        want = DPMS_ON;
    else if (strcmp(a[0], "off") == 0)
        want = DPMS_OFF;
    else if (strcmp(a[0], "toggle") == 0)
        want = DPMS_TOGGLE;
    else {
        warnx("dpms: expected on|off|toggle, got '%s'", a[0]);
        return false;
    }

    const char* target = (a[1] != NULL && a[1][0] != '\0') ? a[1] : NULL;

    struct DpmsCtx ctx = {0};
    ctx.display = connect_display();
    if (ctx.display == NULL) return false;

    bool ok = false;
    ctx.registry = wl_display_get_registry(ctx.display);
    if (ctx.registry == NULL) {
        warnx("could not get wl_registry");
    } else {
        wl_registry_add_listener(ctx.registry, &dpms_registry_listener, &ctx);
        ok = dpms_run(&ctx, target, want);
    }

    dpms_ctx_cleanup(&ctx);
    return ok;
}

#define CMD_NOARG(fn, req)                                                     \
    static bool fn(struct Nctl* n, char* const* a) {                           \
        (void)a;                                                               \
        n->our_serial = n->next_serial++;                                      \
        req(n->controller, n->our_serial);                                     \
        return true;                                                           \
    }
#define CMD_UINT(fn, what, req)                                                \
    static bool fn(struct Nctl* n, char* const* a) {                           \
        uint32_t x;                                                            \
        if (!parse_uint(what, a[0], &x)) return false;                         \
        n->our_serial = n->next_serial++;                                      \
        req(n->controller, n->our_serial, x);                                  \
        return true;                                                           \
    }

CMD_NOARG(cmd_cycle_layout, nullspace_controller_v1_cycle_layout)
CMD_NOARG(cmd_win_focused_close, nullspace_controller_v1_window_close_focused)
CMD_NOARG(cmd_binding_help, nullspace_controller_v1_show_binding_help)
CMD_NOARG(cmd_restore_last, nullspace_controller_v1_restore_last)
CMD_UINT(cmd_switch_space, "space", nullspace_controller_v1_switch_space)
CMD_UINT(cmd_move_to_space, "space", nullspace_controller_v1_move_to_space)
CMD_UINT(cmd_win_activate, "window id", nullspace_controller_v1_window_activate)
CMD_UINT(cmd_win_close, "window id", nullspace_controller_v1_window_close)
CMD_UINT(cmd_win_minimize, "window id", nullspace_controller_v1_window_minimize)

static bool cmd_config(struct Nctl* n, char* const* a) {
    (void)a;

    n->print_key = true;
    n->our_serial = n->next_serial++;
    nullspace_controller_v1_dump_config(n->controller, n->our_serial);
    return true;
}

// This is a no-op now after we connect to the controller.
static bool cmd_ping(struct Nctl* n, char* const* a) {
    (void)n;
    (void)a;

    puts("pong");
    return true;
}

static bool cmd_get(struct Nctl* n, char* const* a) {
    n->print_key = false;
    n->our_serial = n->next_serial++;
    nullspace_controller_v1_get_config(n->controller, n->our_serial, a[0]);
    return true;
}

static bool cmd_set(struct Nctl* n, char* const* a) {
    n->print_key = true;
    n->our_serial = n->next_serial++;
    nullspace_controller_v1_set_config(
        n->controller, n->our_serial, a[0], a[1]
    );

    return true;
}

static const struct {
    const char* name;
    void (*request)(struct nullspace_controller_v1*, uint32_t);
} INSPECT[] = {
    {  "spaces",         nullspace_controller_v1_list_spaces},
    { "layouts",        nullspace_controller_v1_list_layouts},
    { "windows", nullspace_controller_v1_list_window_details},
    { "devices",        nullspace_controller_v1_list_devices},
    {"displays",       nullspace_controller_v1_list_displays},
    {  "layers",         nullspace_controller_v1_list_layers},
};

static bool cmd_inspect(struct Nctl* n, char* const* a) {
    if (strcmp(a[0], "space") == 0) {
        uint32_t x;
        if (a[1] == NULL) {
            warnx("usage: nctl inspect space <n>");
            return false;
        }

        if (!parse_uint("space", a[1], &x)) return false;
        n->our_serial = n->next_serial++;
        nullspace_controller_v1_get_space_info(n->controller, n->our_serial, x);
        return true;
    }

    for (size_t i = 0; i < nitems(INSPECT); i++) {
        if (strcmp(a[0], INSPECT[i].name) != 0) continue;
        if (a[1] != NULL) {
            warnx("inspect %s takes no arguments", a[0]);
            return false;
        }

        n->our_serial = n->next_serial++;
        INSPECT[i].request(n->controller, n->our_serial);
        return true;
    }

    warnx("unknown inspect topic '%s'", a[0]);
    return false;
}

static bool cmd_list(struct Nctl* n, char* const* a);

static const struct Cmd COMMANDS[] = {
    {                         "ping",   "check compositor connection", 0, 0, false,cmd_ping                                                                                   },
    {                    "get <key>",             "query a state key", 1, 1, false,          cmd_get},
    {            "set <key> <value>",    "set a runtime config value", 2, 2, false,          cmd_set},
    {                       "config",       "dump all readable state", 0, 0, false,       cmd_config},
    {                 "list [topic]",       "show available commands", 0, 1,  true,         cmd_list},
    {             "switch-space <n>",             "switch to a space", 1, 1, false, cmd_switch_space},
    {            "move-to-space <n>",       "move the focused window", 1, 1, false,
     cmd_move_to_space                                                                              },
    {                 "cycle-layout",      "cycle to the next layout", 0, 0, false, cmd_cycle_layout},
    {                 "binding-help",   "show on-screen key bindings", 0, 0, false,
     cmd_binding_help                                                                               },
    {         "window-activate <id>",      "focus and raise a window", 1, 1, false,
     cmd_win_activate                                                                               },
    {         "window-close-focused",        "close a focused window", 0, 0, false,
     cmd_win_focused_close                                                                          },
    {            "window-close <id>",                "close a window", 1, 1, false,    cmd_win_close},
    {         "window-minimize <id>",   "move a window to scratchpad", 1, 1, false,
     cmd_win_minimize                                                                               },
    {                 "restore-last", "restore last minimized window", 0, 0, false,
     cmd_restore_last                                                                               },
    {"dpms <on|off|toggle> [output]",           "change output state", 1, 2,  true,
     cmd_dpms                                                                                       },
    {              "inspect <topic>",    "show live compositor state", 1, 2, false,      cmd_inspect},
};

static void list_commands(const struct Style* st) {
    put_heading(st, "COMMANDS");
    for (size_t i = 0; i < nitems(COMMANDS); i++) {
        fputs("  ", stdout);
        put_col(COMMANDS[i].usage, 31);
        fputs(COMMANDS[i].desc, stdout);
        putchar('\n');
    }

    putchar('\n');
}

static void list_layouts(const struct Style* st) {
    put_heading(st, "LAYOUTS");
    for (size_t i = 0; i < nitems(layout_table); i++) {
        fputs("  ", stdout);
        put_col(layout_table[i].name, 10);
        fputs(layout_table[i].desc, stdout);
        putchar('\n');
    }

    putchar('\n');
}

static void list_keys(
    const struct Style* st, const char* title, enum IpcAccess want,
    bool show_range
) {
    size_t name_w = 0, desc_w = 0;
    put_heading(st, title);
    for (size_t i = 0; i < nitems(nctl_keys); i++) {
        const struct IpcKeyMeta* k = &nctl_keys[i];
        if ((k->access & want) == 0) continue;
        size_t n = strlen(k->name);
        if (n > name_w) name_w = n;
        if (show_range) {
            n = strlen(k->desc);
            if (n > desc_w) desc_w = n;
        }
    }

    for (size_t i = 0; i < nitems(nctl_keys); i++) {
        const struct IpcKeyMeta* k = &nctl_keys[i];
        if ((k->access & want) == 0) continue;

        fputs("  ", stdout);
        put_col(k->name, name_w);
        putchar(' ');

        if (show_range && k->range != NULL) {
            put_col(k->desc, desc_w);
            fputs("[", stdout);
            fputs(k->range, stdout);
            fputs("]", stdout);
        } else {
            fputs(k->desc, stdout);
        }

        putchar('\n');
    }

    putchar('\n');
}

static bool cmd_list(struct Nctl* n, char* const* a) {
    (void)n;

    const char* topic = a[0];
    const struct Style style = style_for(STDOUT_FILENO);
    const bool all = (topic == NULL || *topic == '\0');
    bool matched = false;

    if (all || strcmp(topic, "commands") == 0) {
        list_commands(&style);
        matched = true;
    }

    if (all || strcmp(topic, "layouts") == 0) {
        list_layouts(&style);
        matched = true;
    }

    if (all || strcmp(topic, "get") == 0) {
        list_keys(&style, "GETTERS", IPC_A_R, false);
        matched = true;
    }

    if (all || strcmp(topic, "set") == 0) {
        list_keys(&style, "SETTERS", IPC_A_W, true);
        matched = true;
    }

    if (!matched) warnx("unknown list topic '%s'", topic);
    return matched;
}

static void usage(FILE* out) {
    const struct Style style = style_for(fileno(out));
    fputs(style.bold, out);
    fputs("nctl", out);
    fputs(style.reset, out);
    fputs(" v", out);
    fputs(VERSION, out);
    fputs(
        "\n"
        "┌─────────┐\n"
        "│NULLSPACE│\n"
        "└─────────┘\n"
        "-  -  -\n"
        "A command line interface for Nullspace compositor.\n"
        "List all available commands with `nctl list`.\n",
        out
    );
}

static const struct Cmd* find_cmd(const char* name) {
    const size_t len = strlen(name);
    for (size_t i = 0; i < nitems(COMMANDS); i++) {
        const char* u = COMMANDS[i].usage;
        if (strncmp(u, name, len) == 0 && (u[len] == '\0' || u[len] == ' '))
            return &COMMANDS[i];
    }

    return NULL;
}

static bool wait_for_reply(struct Nctl* n) {
    if (wl_display_roundtrip(n->display) < 0) {
        display_failed(n->display);
        return false;
    }

    if (n->got_error) return false;

    if (!n->got_done) {
        warnx("compositor did not send 'done' for serial %u", n->our_serial);
        return false;
    }

    return true;
}

static bool run(struct Nctl* n, int argc, char* const* argv) {
    const struct Cmd* c = find_cmd(argv[0]);
    if (c == NULL) {
        warnx("unknown command '%s' (see `nctl list commands`)", argv[0]);
        return false;
    }

    if (argc - 1 < c->min_args || argc - 1 > c->max_args) {
        warnx("usage: nctl %s", c->usage);
        return false;
    }

    if (c->offline) return c->run(n, argv + 1);

    if (!ctl_connect(n)) goto out;

    const uint32_t before = n->next_serial;
    if (!c->run(n, argv + 1)) goto out;

    if (n->next_serial != before) {
        if (!wait_for_reply(n)) goto out;
    }

out:
    if (n->controller != NULL) nullspace_controller_v1_destroy(n->controller);
    if (n->display != NULL) wl_display_disconnect(n->display);
    return !n->got_error && n->controller != NULL;
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0
        || strcmp(argv[1], "help") == 0) {
        usage(stdout);
        return EXIT_SUCCESS;
    }

    struct Nctl n = {0};
    n.next_serial = 1; // serial 0 is reserved for broadcasts
    const bool ok = run(&n, argc - 1, argv + 1);

    if (fflush(stdout) != 0 || ferror(stdout)) {
        warn("stdout");
        return EXIT_FAILURE;
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
