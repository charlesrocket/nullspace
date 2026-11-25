#include "controller.h"

#include "background.h"
#include "config.h"
#include "decoration.h"
#include "help.h"
#include "icon.h"
#include "input.h"
#include "layer.h"
#include "layout/layout.h"
#include "manager.h"
#include "output.h"
#include "seat.h"
#include "server.h"
#include "window.h"

#include <drm_fourcc.h>
#include <ns-util.h>
#include <nullspace-controller-v1-client-protocol.h>
#include <nullspace/keys.h>
#include <nullspace/layouts.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <wayland-protocols/content-type-v1-enum.h>
#include <wlr-layer-shell-unstable-v1-protocol.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#ifdef XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "nullspace-controller-v1-protocol.h"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>

struct CtlClient {
    struct wl_resource* res;
    struct wl_list link;
};

static struct wl_list ctl_clients = {
    .prev = &ctl_clients,
    .next = &ctl_clients,
};

static void ctl_client_destroy(struct wl_resource* res) {
    struct CtlClient* c = wl_resource_get_user_data(res);
    if (c == NULL) return;
    wl_list_remove(&c->link);
    free(c);
}

struct CtlKey {
    const char* name;
    enum IpcType type;
    enum IpcAccess access;
    const char* range;
    const char* desc;
    void* ctx;
    enum IpcAfter after;
    int min_i, max_i;
    float min_f, max_f;
};

#define STR_(x)         #x
#define STR(x)          STR_(x)
#define RANGE_STR(a, b) STR(a) "," STR(b)

#define ROW_INT(nm, acc, aft, dsc, fld, mn, mx)                                \
    {(nm),      IPC_TYPE_INT,  (acc), RANGE_STR(mn, mx),                       \
     (dsc),     (void*)&(fld), (aft), (int)(mn),                               \
     (int)(mx), 0.0f,          0.0f},
#define ROW_FLOAT(nm, acc, aft, dsc, fld, mn, mx)                              \
    {(nm),  IPC_TYPE_FLOAT, (acc),      RANGE_STR(mn, mx),                     \
     (dsc), (void*)&(fld),  (aft),      0,                                     \
     0,     (float)(mn),    (float)(mx)},
#define ROW_BOOL(nm, acc, aft, rng, dsc, fld)                                  \
    {(nm), IPC_TYPE_BOOL, (acc), (rng), (dsc), (void*)&(fld), (aft), 0,        \
     0,    0.0f,          0.0f},
#define ROW_STR(nm, acc, aft, rng, dsc, fld)                                   \
    {(nm), IPC_TYPE_STR, (acc), (rng), (dsc), NULL, (aft), 0, 0, 0.0f, 0.0f},
#define ROW_SPECIAL(nm, acc, rng, dsc)                                         \
    {(nm), IPC_TYPE_SPECIAL, (acc), (rng), (dsc),                              \
     NULL, IPC_AFTER_NONE,   0,     0,     0.0f,                               \
     0.0f},

static const struct CtlKey ctl_keys[] = {KEYS_TABLE(
    ROW_INT, ROW_FLOAT, ROW_BOOL, ROW_STR, ROW_SPECIAL
) KEYS_TABLE_WALLPAPER(ROW_INT, ROW_FLOAT, ROW_BOOL, ROW_STR, ROW_SPECIAL)};

#define NKEYS (sizeof ctl_keys / sizeof ctl_keys[0])

static const struct CtlKey* find_key(const char* name) {
    for (size_t i = 0; i < NKEYS; i++)
        if (strcmp(ctl_keys[i].name, name) == 0) return &ctl_keys[i];
    return NULL;
}

static void apply_after(enum IpcAfter a) {
    switch (a) {
        case IPC_AFTER_NONE: break;
        case IPC_AFTER_MANAGE: wm_mark_layout_dirty(); break;
        case IPC_AFTER_INPUT: libinput_reconfigure(); break;
        case IPC_AFTER_WALLPAPER:
            wm_invalidate_wallpaper();
            wm_mark_layout_dirty();
            break;
    }
}

static float* deco_color_field(const char* name) {
    if (strcmp(name, "deco_color_active") == 0) return wm.deco_color_active;
    if (strcmp(name, "deco_color_inactive") == 0) return wm.deco_color_inactive;
    if (strcmp(name, "deco_color_button_active") == 0)
        return wm.deco_color_button_active;

    if (strcmp(name, "deco_color_button_inactive") == 0)
        return wm.deco_color_button_inactive;

    return NULL;
}

static void color_to_hex(const float c[4], char out[10]) {
    unsigned int r = (unsigned int)(c[0] * 255.0f + 0.5f);
    unsigned int g = (unsigned int)(c[1] * 255.0f + 0.5f);
    unsigned int b = (unsigned int)(c[2] * 255.0f + 0.5f);
    unsigned int a = (unsigned int)(c[3] * 255.0f + 0.5f);
    if (a == 255)
        snprintf(out, 10, "#%02x%02x%02x", r, g, b);
    else
        snprintf(out, 10, "#%02x%02x%02x%02x", r, g, b, a);
}

static int space_window_count(int space) {
    struct Output* o = output_focused();
    if (o == NULL) return 0;
    int n = 0;
    struct Window* w;
    wl_list_for_each(w, &wm.windows, link) {
        if (!w->closed && w->mapped && w->output == o && w->space == space) n++;
    }

    return n;
}

// 0 success, -2 "use get_space_info", -1 unknown
static int special_get(const char* name, char* b, size_t n) {
    if (strcmp(name, "layout") == 0) {
        int r = snprintf(b, n, "%s", ns_layout_name(wm.layout));
        return r >= 0 ? 0 : -1;
    }

    if (strcmp(name, "current_space") == 0) {
        struct Output* o = output_focused();
        int r = snprintf(b, n, "%d", o ? o->current_space : 0);
        return r >= 0 ? 0 : -1;
    }

    if (strcmp(name, "focused_title") == 0) {
        struct Window* w = window_focused();
        int r = snprintf(b, n, "%s", (w && w->title) ? w->title : "");
        return r >= 0 ? 0 : -1;
    }

    if (strcmp(name, "focused_app_id") == 0) {
        struct Window* w = window_focused();
        int r = snprintf(b, n, "%s", (w && w->app_id) ? w->app_id : "");
        return r >= 0 ? 0 : -1;
    }

    if (strcmp(name, "mfact") == 0) {
        int r = snprintf(b, n, "%g", (double)wm.mfact);
        return r >= 0 ? 0 : -1;
    }

    if (strcmp(name, "wallpaper_path") == 0) {
        int r =
            snprintf(b, n, "%s", wm.wallpaper.path ? wm.wallpaper.path : "");
        return r >= 0 ? 0 : -1;
    }

    if (strcmp(name, "kb_layout") == 0) {
        const char* s = wm.kb_layout ? wm.kb_layout : "";
        int r = snprintf(b, n, "%s", s);
        return r >= 0 ? 0 : -1;
    }

    if (strcmp(name, "space_window_count") == 0
        || strcmp(name, "space_urgent") == 0) {
        return -2;
    }

    float* c = deco_color_field(name);
    if (c) {
        char hex[10];
        color_to_hex(c, hex);
        int r = snprintf(b, n, "%s", hex);
        return r >= 0 ? 0 : -1;
    }

    return -1;
}

static bool special_set(const char* name, const char* v) {
    if (strcmp(name, "mfact") == 0) {
        float x;
        if (!ns_parse_float(v, &x) || x <= 0.0f || x >= 1.0f) return false;
        wm.mfact = x;
        wm_mark_layout_dirty();
        return true;
    }

    if (strcmp(name, "wallpaper_path") == 0) {
        if (*v == '\0') return false;
        return wm_set_wallpaper_path(v);
    }

    float* c = deco_color_field(name);
    if (c) {
        float tmp[4];
        if (!ns_parse_hex_color(v, tmp)) return false;
        memcpy(c, tmp, sizeof tmp);
        deco_refresh_colors();
        wm_mark_layout_dirty();
        return true;
    }

    return false;
}

static int key_get(const struct CtlKey* k, char* b, size_t n) {
    switch (k->type) {
        case IPC_TYPE_INT:
            {
                int r = snprintf(b, n, "%d", *(const int*)k->ctx);
                return r >= 0 ? 0 : -1;
            }
        case IPC_TYPE_FLOAT:
            {
                int r = snprintf(b, n, "%g", (double)*(const float*)k->ctx);
                return r >= 0 ? 0 : -1;
            }
        case IPC_TYPE_BOOL:
            {
                bool v = *(const bool*)k->ctx;
                int r = snprintf(b, n, "%s", v ? "true" : "false");
                return r >= 0 ? 0 : -1;
            }
        case IPC_TYPE_STR:
        case IPC_TYPE_SPECIAL: return special_get(k->name, b, n);
    }

    return -1;
}

static bool
key_set(const struct CtlKey* k, const char* v, char* err, size_t errlen) {
    switch (k->type) {
        case IPC_TYPE_INT:
            {
                long x;
                if (!ns_parse_int(v, &x)) {
                    snprintf(err, errlen, "expected int, got '%s'", v);
                    return false;
                }

                if (x < k->min_i || x > k->max_i) {
                    snprintf(
                        err, errlen, "%ld out of range [%d, %d]", x, k->min_i,
                        k->max_i
                    );

                    return false;
                }

                *(int*)k->ctx = (int)x;
                apply_after(k->after);
                return true;
            }
        case IPC_TYPE_FLOAT:
            {
                float x;
                if (!ns_parse_float(v, &x)) {
                    snprintf(err, errlen, "expected float, got '%s'", v);
                    return false;
                }

                if (x < k->min_f || x > k->max_f) {
                    snprintf(
                        err, errlen, "%g out of range [%g, %g]", (double)x,
                        (double)k->min_f, (double)k->max_f
                    );

                    return false;
                }

                *(float*)k->ctx = x;
                apply_after(k->after);
                return true;
            }
        case IPC_TYPE_BOOL:
            {
                bool x;
                if (strcmp(v, "true") == 0)
                    x = true;
                else if (strcmp(v, "false") == 0)
                    x = false;
                else {
                    snprintf(
                        err, errlen, "expected bool (true|false), got '%s'", v
                    );

                    return false;
                }

                *(bool*)k->ctx = x;
                apply_after(k->after);
                return true;
            }
        case IPC_TYPE_STR: snprintf(err, errlen, "read-only"); return false;
        case IPC_TYPE_SPECIAL:
            {
                if (strcmp(k->name, "layout") == 0) {
                    snprintf(
                        err, errlen,
                        "use set_layout request (config_value has different "
                        "semantics)"
                    );

                    return false;
                }

                if (strcmp(k->name, "current_space") == 0) {
                    snprintf(
                        err, errlen,
                        "use switch_space request (config_value has different "
                        "semantics)"
                    );

                    return false;
                }

                if (!special_set(k->name, v)) {
                    snprintf(
                        err, errlen, "invalid value '%s' for '%s'", v, k->name
                    );

                    return false;
                }

                return true;
            }
    }

    snprintf(err, errlen, "unknown key type");
    return false;
}

static void send_error(
    struct wl_resource* res, uint32_t serial, uint32_t code, const char* msg
) {
    nullspace_controller_v1_send_error(res, serial, code, msg);
    nullspace_controller_v1_send_done(res, serial);
}

static void reply_done(struct wl_resource* res, uint32_t serial) {
    nullspace_controller_v1_send_done(res, serial);
}

typedef void (*EmitFn)(struct wl_resource* res, void* data);

static void broadcast(EmitFn fn, void* data) {
    struct CtlClient* c;
    wl_list_for_each(c, &ctl_clients, link) {
        fn(c->res, data);
        nullspace_controller_v1_send_done(c->res, 0);
    }
}

static void reply_and_broadcast(
    struct wl_resource* sender, uint32_t serial, EmitFn fn, void* data
) {
    fn(sender, data);
    nullspace_controller_v1_send_done(sender, serial);

    struct CtlClient* c;
    wl_list_for_each(c, &ctl_clients, link) {
        if (c->res == sender) continue;
        fn(c->res, data);
        nullspace_controller_v1_send_done(c->res, 0);
    }
}

static bool
send_value_to(struct wl_resource* res, uint32_t serial, const char* key) {
    const struct CtlKey* k = find_key(key);
    char msg[128];

    if (k == NULL) {
        snprintf(msg, sizeof msg, "unknown key '%s'", key);
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_UNKNOWN_KEY, msg
        );

        return false;
    }

    char buf[256];
    int rc = key_get(k, buf, sizeof buf);
    if (rc == -2) {
        snprintf(
            msg, sizeof msg, "'%s' has per-space values; use get_space_info",
            key
        );

        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_NOT_SUPPORTED, msg
        );

        return false;
    }

    if (rc != 0) {
        snprintf(msg, sizeof msg, "could not read '%s'", key);
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_UNKNOWN_KEY, msg
        );

        return false;
    }

    nullspace_controller_v1_send_config_value(res, key, buf);
    return true;
}

static void send_space_info(struct wl_resource* res, int index) {
    struct Output* o = output_focused();
    if (o == NULL) return;

    nullspace_controller_v1_send_space_info(
        res, (uint32_t)index, (uint32_t)space_window_count(index),
        o->space_urgent[index] ? 1u : 0u,
        (uint32_t)(o->current_space == index ? 1 : 0)
    );
}

static void send_layout_info(struct wl_resource* res) {
#define LAYOUT_ROW(id, label, desc)                                            \
    nullspace_controller_v1_send_layout_info(                                  \
        res, (label), (desc), (uint32_t)(wm.layout == (id) ? 1 : 0)            \
    );

    LAYOUTS_TABLE(LAYOUT_ROW)
#undef LAYOUT_ROW
}

struct OneKey {
    const char* key;
};

static void bc_key(struct wl_resource* res, void* data) {
    const struct CtlKey* k = find_key(((struct OneKey*)data)->key);
    if (k == NULL) return;

    char buf[256];
    if (key_get(k, buf, sizeof buf) != 0) return;

    nullspace_controller_v1_send_config_value(res, k->name, buf);
}

static void bc_spaces(struct wl_resource* res, void* data) {
    (void)data;

    for (int i = 0; i < SPACE_COUNT; i++) send_space_info(res, i);
}

static void bc_layout(struct wl_resource* res, void* data) {
    (void)data;

    send_layout_info(res);
}

static void bc_focused_meta(struct wl_resource* res, void* data) {
    (void)data;

    struct Window* w = window_focused();
    nullspace_controller_v1_send_config_value(
        res, "focused_title", (w && w->title) ? w->title : ""
    );

    nullspace_controller_v1_send_config_value(
        res, "focused_app_id", (w && w->app_id) ? w->app_id : ""
    );
}

static void bc_spaces_and_focus(struct wl_resource* res, void* data) {
    bc_spaces(res, data);
    bc_focused_meta(res, data);
}

struct RemovedId {
    uint32_t id;
};

static int window_pid(const struct Window* w) {
#ifdef XWAYLAND
    if (w->is_xwayland) {
        return w->xwayland_surface != NULL ? (int)w->xwayland_surface->pid : 0;
    }
#endif
    if (w->xdg_toplevel == NULL || w->xdg_toplevel->base == NULL
        || w->xdg_toplevel->base->surface == NULL
        || w->xdg_toplevel->base->surface->resource == NULL)
        return 0;

    struct wl_client* client =
        wl_resource_get_client(w->xdg_toplevel->base->surface->resource);
    if (client == NULL) return 0;

    pid_t pid = 0;
    uid_t uid;
    gid_t gid;
    wl_client_get_credentials(client, &pid, &uid, &gid);
    return (int)pid;
}

static const char* content_type_name(enum wp_content_type_v1_type t) {
    switch (t) {
        case WP_CONTENT_TYPE_V1_TYPE_PHOTO: return "photo";
        case WP_CONTENT_TYPE_V1_TYPE_VIDEO: return "video";
        case WP_CONTENT_TYPE_V1_TYPE_GAME: return "game";
        case WP_CONTENT_TYPE_V1_TYPE_NONE:
        default: return "none";
    }
}

static void send_window_detail(
    struct wl_resource* res, struct Window* w, struct Window* focused
) {
    if (w == NULL || w->closed) return;

    const char* monitor = NULL;
    if (w->output != NULL && w->output->wlr_output != NULL)
        monitor = w->output->wlr_output->name;

    const char* ftl_id =
        (w->foreign_handle != NULL && w->foreign_handle->identifier != NULL)
            ? w->foreign_handle->identifier
            : "";

    struct wlr_surface* surface = window_wlr_surface(w);

    enum wp_content_type_v1_type ct = WP_CONTENT_TYPE_V1_TYPE_NONE;
    if (server.content_type_manager != NULL && surface != NULL)
        ct = wlr_surface_get_content_type_v1(
            server.content_type_manager, surface
        );

    enum Layout wlayout = wm.layout;

    uint32_t state = 0;
    if (w->mapped) state |= NULLSPACE_CONTROLLER_V1_WINDOW_STATE_MAPPED;
    if (w == focused) state |= NULLSPACE_CONTROLLER_V1_WINDOW_STATE_FOCUSED;
    if (w->fullscreen) state |= NULLSPACE_CONTROLLER_V1_WINDOW_STATE_FULLSCREEN;
    if (w->maximized) state |= NULLSPACE_CONTROLLER_V1_WINDOW_STATE_MAXIMIZED;
    if (w->minimized) state |= NULLSPACE_CONTROLLER_V1_WINDOW_STATE_MINIMIZED;
    if (window_is_transient(w) || !layouts_is_tiled(wlayout))
        state |= NULLSPACE_CONTROLLER_V1_WINDOW_STATE_FLOATING;
#ifdef XWAYLAND
    if (w->is_xwayland) state |= NULLSPACE_CONTROLLER_V1_WINDOW_STATE_XWAYLAND;
#endif

    uint32_t fx = 0;
    if (w->blur_enabled)
        fx |= NULLSPACE_CONTROLLER_V1_WINDOW_FX_STATE_BLUR_RULE;
    if (surface && background_effect_surface_registered(surface))
        fx |= NULLSPACE_CONTROLLER_V1_WINDOW_FX_STATE_EXT_SURFACE;
    if (surface && background_effect_surface_active(surface))
        fx |= NULLSPACE_CONTROLLER_V1_WINDOW_FX_STATE_EXT_BLUR;
    if (w->decoration && w->decoration->blur_win)
        fx |= NULLSPACE_CONTROLLER_V1_WINDOW_FX_STATE_BLUR;
    if (w->decoration && w->decoration->shadow)
        fx |= NULLSPACE_CONTROLLER_V1_WINDOW_FX_STATE_SHADOW;

    nullspace_controller_v1_send_window_detail(
        res, (uint32_t)w->id, (int32_t)window_pid(w), ftl_id,
        w->app_id ? w->app_id : "", w->title ? w->title : "",
        monitor ? monitor : "", (int32_t)w->space, (int32_t)w->x, (int32_t)w->y,
        (int32_t)w->width, (int32_t)w->height, state, content_type_name(ct), fx
    );
}

static const char* input_device_type_name(enum wlr_input_device_type t) {
    switch (t) {
        case WLR_INPUT_DEVICE_KEYBOARD: return "keyboard";
        case WLR_INPUT_DEVICE_POINTER: return "pointer";
        case WLR_INPUT_DEVICE_TOUCH: return "touch";
        case WLR_INPUT_DEVICE_TABLET: return "tablet";
        case WLR_INPUT_DEVICE_TABLET_PAD: return "tablet_pad";
        case WLR_INPUT_DEVICE_SWITCH: return "switch";
    }

    return "unknown";
}

static void emit_device(struct wlr_input_device* dev, void* data) {
    struct wl_resource* res = data;
    nullspace_controller_v1_send_device_info(
        res, input_device_type_name(dev->type), dev->name ? dev->name : ""
    );
}

static const char* layer_shell_name(enum zwlr_layer_shell_v1_layer l) {
    switch (l) {
        case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return "background";
        case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: return "bottom";
        case ZWLR_LAYER_SHELL_V1_LAYER_TOP: return "top";
        case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY: return "overlay";
    }

    return "unknown";
}

static const char*
layer_keyboard_name(enum zwlr_layer_surface_v1_keyboard_interactivity k) {
    switch (k) {
        case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE: return "none";
        case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE:
            return "exclusive";
        case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND:
            return "on_demand";
    }

    return "unknown";
}

static bool locked(struct wl_resource* res, uint32_t serial) {
    if (server.locked) {
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_LOCKED, "session locked"
        );

        return true;
    }

    return false;
}

static void ctl_destroy_req(struct wl_client* c, struct wl_resource* r) {
    (void)c;

    wl_resource_destroy(r);
}

static void ctl_get_config(
    struct wl_client* c, struct wl_resource* res, uint32_t serial,
    const char* key
) {
    (void)c;

    if (send_value_to(res, serial, key)) reply_done(res, serial);
}

static void ctl_set_config(
    struct wl_client* c, struct wl_resource* res, uint32_t serial,
    const char* key, const char* value
) {
    (void)c;

    if (locked(res, serial)) return;

    const struct CtlKey* k = find_key(key);
    char msg[256];

    if (k == NULL) {
        snprintf(msg, sizeof msg, "unknown key '%s'", key);
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_UNKNOWN_KEY, msg
        );

        return;
    }

    if ((k->access & IPC_A_W) == 0) {
        snprintf(msg, sizeof msg, "'%s' is read-only", key);
        send_error(res, serial, NULLSPACE_CONTROLLER_V1_STATUS_READ_ONLY, msg);
        return;
    }

    char detail[128] = {0};
    if (!key_set(k, value, detail, sizeof detail)) {
        snprintf(msg, sizeof msg, "'%s': %s", key, detail);
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_INVALID_VALUE, msg
        );

        return;
    }

    wm_mark_layout_dirty();

    struct OneKey ok = {.key = key};
    reply_and_broadcast(res, serial, bc_key, &ok);
}

static void
ctl_dump_config(struct wl_client* c, struct wl_resource* res, uint32_t serial) {
    (void)c;

    for (size_t i = 0; i < NKEYS; i++) {
        if ((ctl_keys[i].access & IPC_A_R) == 0) continue;
        char buf[256];
        if (key_get(&ctl_keys[i], buf, sizeof buf) != 0) continue;
        nullspace_controller_v1_send_config_value(res, ctl_keys[i].name, buf);
    }

    reply_done(res, serial);
}

static void ctl_list_config_keys(
    struct wl_client* c, struct wl_resource* res, uint32_t serial,
    uint32_t access
) {
    (void)c;

    for (size_t i = 0; i < NKEYS; i++) {
        const struct CtlKey* k = &ctl_keys[i];
        if ((k->access & access) == 0) continue;
        nullspace_controller_v1_send_config_key(
            res, k->name, (uint32_t)k->type, (uint32_t)k->access,
            k->range ? k->range : "", k->desc ? k->desc : ""
        );
    }

    reply_done(res, serial);
}

static void ctl_switch_space(
    struct wl_client* c, struct wl_resource* res, uint32_t serial,
    uint32_t index
) {
    (void)c;

    if (locked(res, serial)) return;
    if (index >= (uint32_t)SPACE_COUNT) {
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_INVALID_VALUE, "space"
        );

        return;
    }

    wm_switch_space((int)index);
    reply_and_broadcast(res, serial, bc_spaces, NULL);
}

static void ctl_move_to_space(
    struct wl_client* c, struct wl_resource* res, uint32_t serial,
    uint32_t index
) {
    (void)c;

    if (locked(res, serial)) return;
    if (index >= (uint32_t)SPACE_COUNT) {
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_INVALID_VALUE, "space"
        );

        return;
    }

    struct Window* w = window_focused();
    if (w == NULL) {
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_NO_FOCUS,
            "no focused window"
        );

        return;
    }

    wm_move_window_to_space(w, (int)index);
    reply_and_broadcast(res, serial, bc_spaces_and_focus, NULL);
}

static void ctl_get_space_info(
    struct wl_client* c, struct wl_resource* res, uint32_t serial,
    uint32_t index
) {
    (void)c;

    if (index >= (uint32_t)SPACE_COUNT) {
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_INVALID_VALUE, "space"
        );

        return;
    }

    send_space_info(res, (int)index);
    reply_done(res, serial);
}

static void
ctl_list_spaces(struct wl_client* c, struct wl_resource* res, uint32_t serial) {
    (void)c;

    if (output_focused() == NULL) {
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_NO_FOCUS,
            "no focused output"
        );

        return;
    }

    for (int i = 0; i < SPACE_COUNT; i++) send_space_info(res, i);
    reply_done(res, serial);
}

static void ctl_cycle_layout(
    struct wl_client* c, struct wl_resource* res, uint32_t serial
) {
    (void)c;

    if (locked(res, serial)) return;
    wm_set_layout((enum Layout)((wm.layout + 1) % ((int)LAYOUT_LAST + 1)));
    reply_and_broadcast(res, serial, bc_layout, NULL);
}

static void ctl_set_layout(
    struct wl_client* c, struct wl_resource* res, uint32_t serial,
    const char* name
) {
    (void)c;

    if (locked(res, serial)) return;

    enum Layout l;
    if (!ns_parse_layout_name(name, &l)) {
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_INVALID_VALUE, name
        );

        return;
    }

    wm_set_layout(l);
    reply_and_broadcast(res, serial, bc_layout, NULL);
}

static void ctl_list_layouts(
    struct wl_client* c, struct wl_resource* res, uint32_t serial
) {
    (void)c;

    send_layout_info(res);
    reply_done(res, serial);
}

static void ctl_close_focused(
    struct wl_client* c, struct wl_resource* res, uint32_t serial
) {
    (void)c;

    if (locked(res, serial)) return;

    struct Window* w = window_focused();
    if (w == NULL) {
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_NO_FOCUS,
            "no focused window"
        );

        return;
    }

    window_close(w);
    reply_done(res, serial);
}

static void ctl_window_activate(
    struct wl_client* c, struct wl_resource* res, uint32_t serial, uint32_t id
) {
    (void)c;

    if (locked(res, serial)) return;

    struct Window* w = window_from_id(id);
    if (w == NULL || w->closed) {
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_NO_SUCH_WINDOW,
            "no such id"
        );

        return;
    }

    if (w->minimized) {
        wm_restore_window(w);
    } else {
        if (w->space_hidden) {
            if (w->output != NULL)
                wm_switch_space_on(w->output, w->space);
            else
                wm_switch_space(w->space);
        }

        seat_focus(w);
        if (!layouts_is_tiled(wm.layout)) window_raise(w);
    }

    reply_and_broadcast(res, serial, bc_focused_meta, NULL);
}

static void ctl_window_close(
    struct wl_client* c, struct wl_resource* res, uint32_t serial, uint32_t id
) {
    (void)c;

    if (locked(res, serial)) return;

    struct Window* w = window_from_id(id);

    if (w->closed) {
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_INVALID_VALUE,
            "window is already closed"
        );

        return;
    }

    if (w == NULL) {
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_NO_SUCH_WINDOW,
            "no such id"
        );

        return;
    }

    window_close(w);
    reply_done(res, serial);
}

static void ctl_window_minimize(
    struct wl_client* c, struct wl_resource* res, uint32_t serial, uint32_t id
) {
    (void)c;

    if (locked(res, serial)) return;

    struct Window* w = window_from_id(id);

    if (w->minimized) {
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_INVALID_VALUE,
            "window is already minimized"
        );

        return;
    }

    if (w == NULL) {
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_NO_SUCH_WINDOW,
            "no such id"
        );

        return;
    }

    wm_minimize_window(w);
    reply_and_broadcast(res, serial, bc_spaces_and_focus, NULL);
}

static void ctl_restore_last(
    struct wl_client* c, struct wl_resource* res, uint32_t serial
) {
    (void)c;

    if (locked(res, serial)) return;

    wm_restore_last_minimized();
    reply_and_broadcast(res, serial, bc_spaces_and_focus, NULL);
}

static void ctl_show_binding_help(
    struct wl_client* c, struct wl_resource* res, uint32_t serial
) {
    (void)c;

    if (server.overview_active) {
        send_error(
            res, serial, NULLSPACE_CONTROLLER_V1_STATUS_NOT_SUPPORTED,
            "overview is active"
        );

        return;
    }

    help_show();
    reply_done(res, serial);
}

static void ctl_list_window_details(
    struct wl_client* c, struct wl_resource* res, uint32_t serial
) {
    (void)c;

    struct Window* focused = window_focused();
    struct Window* w;
    wl_list_for_each(w, &wm.windows, link) if (!w->closed)
        send_window_detail(res, w, focused);

    reply_done(res, serial);
}

static void ctl_list_devices(
    struct wl_client* c, struct wl_resource* res, uint32_t serial
) {
    (void)c;

    seat_foreach_input_device(emit_device, res);
    reply_done(res, serial);
}

static void ctl_list_displays(
    struct wl_client* c, struct wl_resource* res, uint32_t serial
) {
    (void)c;

    struct Output* o;
    wl_list_for_each(o, &server.outputs, link) {
        if (o->removed) continue;

        const struct wlr_output* wlr = o->wlr_output;
        if (wlr == NULL) continue;

        char scale_buf[16];
        snprintf(scale_buf, sizeof scale_buf, "%g", (double)wlr->scale);
        nullspace_controller_v1_send_display_info(
            res, wlr->name ? wlr->name : "", wlr->make ? wlr->make : "",
            wlr->model ? wlr->model : "", wlr->serial ? wlr->serial : "",
            o->pos_x, o->pos_y, o->width, o->height, wlr->phys_width,
            wlr->phys_height, scale_buf, wlr->refresh, wlr->enabled ? 1u : 0u,
            o->current_space, o->area_x, o->area_y, o->area_width,
            o->area_height
        );
    }

    reply_done(res, serial);
}

static void
ctl_list_layers(struct wl_client* c, struct wl_resource* res, uint32_t serial) {
    (void)c;

    struct LayerSurface* ls;
    wl_list_for_each(ls, &wm.layer_surfaces, link) {
        const struct wlr_layer_surface_v1* surf = ls->layer_surface;
        if (surf == NULL) continue;
        const char* out_name =
            (ls->output != NULL && ls->output->wlr_output != NULL)
                ? ls->output->wlr_output->name
                : "";

        int x = 0, y = 0;
        if (ls->mapped && ls->scene_tree != NULL)
            wlr_scene_node_coords(&ls->scene_tree->node, &x, &y);

        nullspace_controller_v1_send_layer_info(
            res, surf->namespace ? surf->namespace : "",
            layer_shell_name(surf->current.layer), out_name,
            ls->mapped ? 1u : 0u, x, y, ls->last_w, ls->last_h,
            surf->current.exclusive_zone,
            layer_keyboard_name(surf->current.keyboard_interactive)
        );
    }

    reply_done(res, serial);
}

static const struct nullspace_controller_v1_interface ctl_impl = {
    .destroy = ctl_destroy_req,
    .get_config = ctl_get_config,
    .set_config = ctl_set_config,
    .dump_config = ctl_dump_config,
    .list_config_keys = ctl_list_config_keys,
    .switch_space = ctl_switch_space,
    .move_to_space = ctl_move_to_space,
    .get_space_info = ctl_get_space_info,
    .list_spaces = ctl_list_spaces,
    .cycle_layout = ctl_cycle_layout,
    .set_layout = ctl_set_layout,
    .list_layouts = ctl_list_layouts,
    .window_close_focused = ctl_close_focused,
    .window_activate = ctl_window_activate,
    .window_close = ctl_window_close,
    .window_minimize = ctl_window_minimize,
    .restore_last = ctl_restore_last,
    .list_window_details = ctl_list_window_details,
    .list_devices = ctl_list_devices,
    .list_displays = ctl_list_displays,
    .list_layers = ctl_list_layers,
    .show_binding_help = ctl_show_binding_help,
};

static void
ctl_bind(struct wl_client* client, void* data, uint32_t version, uint32_t id) {
    (void)data;

    struct wl_resource* res = wl_resource_create(
        client, &nullspace_controller_v1_interface, (int)version, id
    );

    if (res == NULL) {
        wl_client_post_no_memory(client);
        return;
    }

    struct CtlClient* c = calloc(1, sizeof *c);
    if (c == NULL) {
        wl_resource_destroy(res);
        wl_client_post_no_memory(client);
        return;
    }

    c->res = res;
    wl_list_insert(&ctl_clients, &c->link);
    wl_resource_set_implementation(res, &ctl_impl, c, ctl_client_destroy);
}

struct Controller* controller_create(struct wl_display* display) {
    struct Controller* c = calloc(1, sizeof *c);
    if (c == NULL) return NULL;

    wl_list_init(&ctl_clients);
    c->global = wl_global_create(
        display, &nullspace_controller_v1_interface, 1, NULL, ctl_bind
    );

    if (c->global == NULL) {
        free(c);
        return NULL;
    }

    return c;
}

void controller_destroy(struct Controller* c) {
    if (c == NULL) return;

    wl_global_destroy(c->global);

    struct CtlClient *cl, *tmp;
    wl_list_for_each_safe(cl, tmp, &ctl_clients, link) {
        wl_resource_set_user_data(cl->res, NULL);
        wl_list_remove(&cl->link);
        free(cl);
    }

    free(c);
}

void controller_notify_spaces(void) { broadcast(bc_spaces, NULL); }
void controller_notify_layout(void) { broadcast(bc_layout, NULL); }

void controller_notify_kb_layout(void) {
    struct OneKey ok = {.key = "kb_layout"};
    broadcast(bc_key, &ok);
}

void controller_notify_focus(void) { broadcast(bc_focused_meta, NULL); }

struct OpenedBatch {
    struct Window* w;
    bool focused;
};

static void bc_opened(struct wl_resource* res, void* data) {
    struct OpenedBatch* b = data;
    send_window_detail(res, b->w, window_focused());
    bc_spaces(res, NULL);
    if (b->focused) bc_focused_meta(res, NULL);
}

void controller_notify_window_opened(struct Window* w) {
    if (w == NULL) return;
    struct OpenedBatch b = {.w = w, .focused = (w == window_focused())};
    broadcast(bc_opened, &b);
}

struct ClosedBatch {
    uint32_t id;
    bool focused_was;
};

static void bc_closed(struct wl_resource* res, void* data) {
    struct ClosedBatch* b = data;
    nullspace_controller_v1_send_window_removed(res, b->id);
    bc_spaces(res, NULL);
    if (b->focused_was) bc_focused_meta(res, NULL);
}

void controller_notify_window_closed(struct Window* w) {
    if (w == NULL) return;
    struct ClosedBatch b = {
        .id = (uint32_t)w->id,
        .focused_was = (w == window_focused()),
    };
    broadcast(bc_closed, &b);
}

struct MetaBatch {
    struct Window* w;
    bool focused;
};

static void bc_meta(struct wl_resource* res, void* data) {
    struct MetaBatch* b = data;
    send_window_detail(res, b->w, window_focused());
    if (b->focused) bc_focused_meta(res, NULL);
}

void controller_notify_window_meta(struct Window* w) {
    if (w == NULL) return;
    struct MetaBatch b = {.w = w, .focused = (w == window_focused())};
    broadcast(bc_meta, &b);
}
