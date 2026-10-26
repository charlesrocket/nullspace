#include "xwayland.h"

#include "config.h"
#include "log.h"
#include "output.h"
#include "server.h"
#include "window.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/xwayland.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/xproto.h>

#define LOG_TOPIC     "xwayland"
#define XWL_NAME_MAX  64
#define XWL_CACHE_MAX 16

enum PrimaryPhase {
    PHASE_IDLE,
    PHASE_RESOURCES, // GetScreenResourcesCurrent
    PHASE_INFO,      // GetOutputInfo replies
};

static struct wl_listener new_surface_listener;
static struct wl_listener ready_listener;

static xcb_connection_t *conn = NULL;
static struct wl_event_source *conn_source = NULL;
static xcb_window_t root = XCB_NONE;

static char display_name[XWL_NAME_MAX];
static char target_name[XWL_NAME_MAX];
static char applied_name[XWL_NAME_MAX];

static char cache_name[XWL_CACHE_MAX][XWL_NAME_MAX];
static xcb_randr_output_t cache_output[XWL_CACHE_MAX];
static int32_t cache_len = 0;
static bool cache_valid = false;
static bool cache_stale = false; // outputs changed while a query was in flight
static bool cache_refresh_attempted = false;

static enum PrimaryPhase phase = PHASE_IDLE;
static xcb_randr_get_screen_resources_current_cookie_t resources_cookie;
static xcb_randr_output_t outputs[XWL_CACHE_MAX];
static xcb_randr_get_output_info_cookie_t info_cookies[XWL_CACHE_MAX];
static int32_t outputs_len = 0;
static int32_t info_idx = 0;

static void xwayland_primary_apply(void);
static void xwayland_primary_start(void);
static int32_t xwayland_primary_ready(int32_t fd, uint32_t mask, void *data);

static bool xwayland_alive(void) {
    return server.xwayland != NULL && server.xwayland->xwm != NULL;
}

static void xwayland_primary_unwatch(void) {
    if (conn_source) {
        wl_event_source_remove(conn_source);
        conn_source = NULL;
    }
}

static void xwayland_primary_disconnect(void) {
    xwayland_primary_unwatch();

    if (conn) {
        xcb_disconnect(conn);
        conn = NULL;
    }

    root = XCB_NONE;
    phase = PHASE_IDLE;
    outputs_len = 0;
    info_idx = 0;
    cache_len = 0;
    cache_valid = false;
}

static void xwayland_primary_close(void) {
    xwayland_primary_disconnect();
    applied_name[0] = '\0';
}

static void xwayland_primary_watch(void) {
    if (!conn || conn_source) { return; }

    conn_source = wl_event_loop_add_fd(
        server.event_loop, xcb_get_file_descriptor(conn), WL_EVENT_READABLE,
        xwayland_primary_ready, NULL
    );

    if (conn_source == NULL) { xwayland_primary_disconnect(); }
}

static bool xwayland_primary_connect(void) {
    if (conn) { return true; }
    if (!display_name[0]) { return false; }

    int32_t screen_num = 0;
    conn = xcb_connect(display_name, &screen_num);

    if (!conn || xcb_connection_has_error(conn)
        || xcb_get_setup(conn) == NULL) {
        xwayland_primary_disconnect();
        return false;
    }

    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
    for (int32_t i = 0; i < screen_num && it.rem; i++) { xcb_screen_next(&it); }

    if (!it.rem) {
        xwayland_primary_disconnect();
        return false;
    }

    root = it.data->root;
    cache_valid = false;
    return true;
}

static void xwayland_primary_build_cache(void) {
    outputs_len = 0;
    info_idx = 0;
    cache_len = 0;
    cache_valid = false;
    cache_stale = false;

    phase = PHASE_RESOURCES;
    resources_cookie = xcb_randr_get_screen_resources_current(conn, root);

    if (xcb_flush(conn) <= 0) { xwayland_primary_disconnect(); }
}

static void xwayland_primary_apply(void) {
    if (!conn) { return; }

    if (!target_name[0]) {
        xwayland_primary_disconnect();
        return;
    }

    if (phase != PHASE_IDLE) { return; }

    if (!cache_valid) {
        xwayland_primary_build_cache();
        return;
    }

    for (int32_t i = 0; i < cache_len; i++) {
        if (strncmp(cache_name[i], target_name, XWL_NAME_MAX) != 0) {
            continue;
        }

        xcb_randr_set_output_primary(conn, root, cache_output[i]);
        xcb_flush(conn);

        snprintf(applied_name, sizeof(applied_name), "%s", target_name);
        cache_refresh_attempted = false;
        log_info("Primary output set to %s", applied_name);

        xwayland_primary_disconnect();
        return;
    }

    if (!cache_refresh_attempted) {
        cache_refresh_attempted = true;
        cache_valid = false;
        xwayland_primary_build_cache();
        return;
    }

    log_warn("Output %s not found among Xwayland outputs", target_name);
    applied_name[0] = '\0';
    xwayland_primary_disconnect();
}

static void xwayland_primary_start(void) {
    if (!xwayland_alive()) { return; }
    if (!target_name[0]) { return; }
    if (strncmp(applied_name, target_name, XWL_NAME_MAX) == 0) { return; }

    if (conn && xcb_connection_has_error(conn)) {
        xwayland_primary_disconnect();
    }

    if (!conn && !xwayland_primary_connect()) { return; }

    xwayland_primary_watch();
    if (!conn) { return; }

    xwayland_primary_apply();
}

static void xwayland_primary_drain_events(void) {
    xcb_generic_event_t *event;
    while ((event = xcb_poll_for_event(conn)) != NULL) {
        if (event->response_type == 0) {
            xcb_generic_error_t *err = (xcb_generic_error_t *)event;
            log_warn(
                "X error %u (major %u, minor %u)", (unsigned)err->error_code,
                (unsigned)err->major_code, (unsigned)err->minor_code
            );
        }

        free(event);
    }
}

static int32_t xwayland_primary_ready(int32_t fd, uint32_t mask, void *data) {
    (void)fd;
    (void)data;

    if (!conn || (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
        || xcb_connection_has_error(conn)) {
        xwayland_primary_disconnect();
        return 0;
    }

    xwayland_primary_drain_events();
    if (xcb_connection_has_error(conn)) {
        xwayland_primary_disconnect();
        return 0;
    }

    if (phase == PHASE_RESOURCES) {
        xcb_randr_get_screen_resources_current_reply_t *resources = NULL;
        xcb_generic_error_t *err = NULL;

        if (!xcb_poll_for_reply(
                conn, resources_cookie.sequence, (void **)&resources, &err
            )) {
            return 0;
        }

        if (err) {
            log_warn("RandR GetScreenResources failed (%u)", err->error_code);
            free(err);
        }

        if (!resources) {
            xwayland_primary_disconnect();
            return 0;
        }

        int32_t len =
            xcb_randr_get_screen_resources_current_outputs_length(resources);
        const xcb_randr_output_t *ids =
            xcb_randr_get_screen_resources_current_outputs(resources);

        if (len > XWL_CACHE_MAX) {
            log_warn("Ignoring %d Xwayland outputs", len - XWL_CACHE_MAX);
            len = XWL_CACHE_MAX;
        }

        outputs_len = 0;
        for (int32_t i = 0; ids && i < len; i++) {
            outputs[outputs_len++] = ids[i];
        }

        free(resources);

        for (int32_t i = 0; i < outputs_len; i++) {
            info_cookies[i] =
                xcb_randr_get_output_info(conn, outputs[i], XCB_CURRENT_TIME);
        }

        if (xcb_flush(conn) <= 0) {
            xwayland_primary_disconnect();
            return 0;
        }

        info_idx = 0;
        cache_len = 0;
        phase = PHASE_INFO;
    }

    if (phase == PHASE_INFO) {
        while (info_idx < outputs_len) {
            xcb_randr_get_output_info_reply_t *info = NULL;
            xcb_generic_error_t *err = NULL;

            if (!xcb_poll_for_reply(
                    conn, info_cookies[info_idx].sequence, (void **)&info, &err
                )) {
                return 0;
            }

            if (err) { free(err); }

            if (!info && xcb_connection_has_error(conn)) {
                xwayland_primary_disconnect();
                return 0;
            }

            if (info) {
                int32_t len = xcb_randr_get_output_info_name_length(info);
                const char *name =
                    (const char *)xcb_randr_get_output_info_name(info);

                if (name && len > 0 && cache_len < XWL_CACHE_MAX) {
                    int32_t copy =
                        len < XWL_NAME_MAX - 1 ? len : XWL_NAME_MAX - 1;

                    memcpy(cache_name[cache_len], name, (size_t)copy);
                    cache_name[cache_len][copy] = '\0';
                    cache_output[cache_len] = outputs[info_idx];
                    cache_len++;
                }

                free(info);
            }

            info_idx++;
        }

        phase = PHASE_IDLE;
        outputs_len = info_idx = 0;

        if (cache_stale) {
            xwayland_primary_build_cache();
            return 0;
        }

        cache_valid = true;
        xwayland_primary_apply();
    }

    return 0;
}

static void xwayland_primary_set(struct Output *out) {
    const char *display =
        server.xwayland ? server.xwayland->display_name : NULL;

    if (!display || !out || !out->wlr_output || !out->wlr_output->name) {
        return;
    }

    if (strncmp(display_name, display, XWL_NAME_MAX) != 0) {
        xwayland_primary_close();
        snprintf(display_name, sizeof(display_name), "%s", display);
    }

    if (strncmp(target_name, out->wlr_output->name, XWL_NAME_MAX) != 0) {
        cache_refresh_attempted = false;
    }

    snprintf(target_name, sizeof(target_name), "%s", out->wlr_output->name);

    if (strncmp(applied_name, target_name, XWL_NAME_MAX) == 0) { return; }

    xwayland_primary_start();
}

void xwayland_primary_update(void) {
    cache_refresh_attempted = false;
    applied_name[0] = '\0';
    cache_valid = false;
    cache_stale = true;

    struct Output *out = output_primary();
    if (out) {
        xwayland_primary_set(out);
    } else {
        target_name[0] = '\0';
        xwayland_primary_disconnect();
    }
}

static void xw_handle_ready(struct wl_listener *l, void *data) {
    (void)l;
    (void)data;

    if (server.xwayland->display_name) {
        setenv("DISPLAY", server.xwayland->display_name, 1);
        log_info("Ready (DISPLAY=%s)", server.xwayland->display_name);
    } else {
        log_info("Ready");
    }

    xwayland_primary_update();
}

struct XWaylandUnmanaged {
    struct wlr_xwayland_surface *xw;
    struct wlr_scene_tree *tree;
    struct wlr_scene_tree *surface_tree;
    struct wl_listener destroy;
    struct wl_listener set_geometry;
    struct wl_listener request_configure;
    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener surface_tree_destroy;
};

static void xw_unmanaged_sync_position(struct XWaylandUnmanaged *u) {
    wlr_scene_node_set_position(&u->tree->node, u->xw->x, u->xw->y);
}

static void xw_unmanaged_set_geometry(struct wl_listener *l, void *data) {
    (void)data;

    struct XWaylandUnmanaged *u = wl_container_of(l, u, set_geometry);
    xw_unmanaged_sync_position(u);
}

static void xw_unmanaged_request_configure(struct wl_listener *l, void *data) {
    (void)data;

    struct XWaylandUnmanaged *u = wl_container_of(l, u, request_configure);

    int16_t x = u->xw->x;
    int16_t y = u->xw->y;
    uint16_t w = u->xw->width;
    uint16_t h = u->xw->height;

    if (w == 0) w = 1;
    if (h == 0) h = 1;

    wlr_xwayland_surface_configure(u->xw, x, y, w, h);
}

static void
xw_unmanaged_surface_tree_destroyed(struct wl_listener *l, void *data) {
    (void)data;

    struct XWaylandUnmanaged *u = wl_container_of(l, u, surface_tree_destroy);
    wl_list_remove(&u->surface_tree_destroy.link);
    u->surface_tree = NULL;
}

static void xw_unmanaged_unbind(struct XWaylandUnmanaged *u) {
    if (u->surface_tree == NULL) return;

    wl_list_remove(&u->surface_tree_destroy.link);
    wlr_scene_node_destroy(&u->surface_tree->node);
    u->surface_tree = NULL;
}

static void xw_unmanaged_bind(struct XWaylandUnmanaged *u) {
    if (u->surface_tree != NULL || u->xw->surface == NULL) return;

    u->surface_tree = wlr_scene_subsurface_tree_create(u->tree, u->xw->surface);
    if (u->surface_tree == NULL) {
        log_warn("Unable to create scene tree for override-redirect window");
        return;
    }

    u->surface_tree_destroy.notify = xw_unmanaged_surface_tree_destroyed;
    wl_signal_add(
        &u->surface_tree->node.events.destroy, &u->surface_tree_destroy
    );
}

static void xw_unmanaged_associate(struct wl_listener *l, void *data) {
    (void)data;

    struct XWaylandUnmanaged *u = wl_container_of(l, u, associate);
    xw_unmanaged_bind(u);
}

static void xw_unmanaged_dissociate(struct wl_listener *l, void *data) {
    (void)data;

    struct XWaylandUnmanaged *u = wl_container_of(l, u, dissociate);
    xw_unmanaged_unbind(u);
}

static void xw_unmanaged_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct XWaylandUnmanaged *u = wl_container_of(l, u, destroy);

    xw_unmanaged_unbind(u);

    wl_list_remove(&u->associate.link);
    wl_list_remove(&u->dissociate.link);
    wl_list_remove(&u->destroy.link);
    wl_list_remove(&u->set_geometry.link);
    wl_list_remove(&u->request_configure.link);

    u->xw->data = NULL;
    wlr_scene_node_destroy(&u->tree->node);
    free(u);
}

static void xwayland_unmanaged_create(struct wlr_xwayland_surface *xw) {
    struct XWaylandUnmanaged *u = calloc(1, sizeof(*u));
    if (u == NULL) {
        wlr_xwayland_surface_close(xw);
        return;
    }

    u->tree = wlr_scene_tree_create(server.overlay_tree);
    if (u->tree == NULL) {
        free(u);
        wlr_xwayland_surface_close(xw);
        return;
    }

    u->xw = xw;
    xw->data = u;

    u->destroy.notify = xw_unmanaged_destroy;
    wl_signal_add(&xw->events.destroy, &u->destroy);

    u->set_geometry.notify = xw_unmanaged_set_geometry;
    wl_signal_add(&xw->events.set_geometry, &u->set_geometry);

    u->request_configure.notify = xw_unmanaged_request_configure;
    wl_signal_add(&xw->events.request_configure, &u->request_configure);

    u->associate.notify = xw_unmanaged_associate;
    wl_signal_add(&xw->events.associate, &u->associate);

    u->dissociate.notify = xw_unmanaged_dissociate;
    wl_signal_add(&xw->events.dissociate, &u->dissociate);

    xw_unmanaged_sync_position(u);
    xw_unmanaged_bind(u); // no-op unless a surface is already associated
}

static void xw_handle_new_surface(struct wl_listener *l, void *data) {
    (void)l;

    struct wlr_xwayland_surface *xw = data;

    if (xw->override_redirect) {
        xwayland_unmanaged_create(xw);
    } else {
        window_create_xwayland(xw);
    }
}

bool xwayland_init(struct wl_display *display) {
    if (!config.xwayland) {
        log_info("Server disabled");
        return false;
    }

    if (server.compositor == NULL) {
        log_warn("Compositor instance missing");
        return false;
    }

    server.xwayland = wlr_xwayland_create(display, server.compositor, true);

    if (server.xwayland == NULL) {
        log_warn("Server creation failed");
        return false;
    }

    if (server.xwayland->display_name != NULL) {
        setenv("DISPLAY", server.xwayland->display_name, 1);
    }

    if (server.seat != NULL) {
        wlr_xwayland_set_seat(server.xwayland, server.seat);
    }

    new_surface_listener.notify = xw_handle_new_surface;
    wl_signal_add(&server.xwayland->events.new_surface, &new_surface_listener);

    ready_listener.notify = xw_handle_ready;
    wl_signal_add(&server.xwayland->events.ready, &ready_listener);

    log_info("Server started");
    return true;
}

void xwayland_finish(void) {
    xwayland_primary_close();
    target_name[0] = '\0';
    display_name[0] = '\0';

    if (server.xwayland != NULL) {
        wl_list_remove(&new_surface_listener.link);
        wl_list_remove(&ready_listener.link);
        wlr_xwayland_destroy(server.xwayland);
        server.xwayland = NULL;
    }
}
