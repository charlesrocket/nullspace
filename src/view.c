#include "view.h"

#include "cursor.h"
#include "include/layouts.h"
#include "ipc.h"
#include "layouts/layout.h"
#include "manager.h"
#include "output.h"
#include "seat.h"
#include "server.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#define LOG_TOPIC "view"

static struct wlr_scene_tree *layer_for_layout(enum Layout layout) {
    return layouts_is_tiled(layout) ? server.tile_tree : server.float_tree;
}

void view_set_activated(struct Window *view, bool activated) {
    wlr_xdg_toplevel_set_activated(view->xdg_toplevel, activated);
    if (view->wlr_handle != NULL) {
        wlr_foreign_toplevel_handle_v1_set_activated(
            view->wlr_handle, activated
        );
    }
}

void view_set_fullscreen(struct Window *view, bool fullscreen) {
    if (view->closed || view->fullscreen == fullscreen) return;
    if (!view->xdg_toplevel->base->initialized) return;

    if (fullscreen) {
        view->fs_x = view->x;
        view->fs_y = view->y;
        view->fs_w = view->width;
        view->fs_h = view->height;
    }

    view->fullscreen = fullscreen;
    wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, fullscreen);

    view->has_target = false;
    view_reparent(view, !layouts_is_tiled(wm.layout));

    if (!fullscreen && !layouts_is_tiled(wm.layout)) { // floating
        // restore from saved
        window_apply_target(
            view, view->fs_x, view->fs_y, view->fs_w, view->fs_h
        );
    }

    if (fullscreen) view_raise(view);
    wm_mark_layout_dirty();
}

static void wlr_handle_request_activate(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *view = wl_container_of(l, view, wlr_request_activate);
    if (view->closed) return;
    if (view->space_hidden) wm_switch_space(view->space);

    seat_focus(view);
}

static void wlr_handle_request_close(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *view = wl_container_of(l, view, wlr_request_close);
    view_close(view);
}

static void wlr_handle_create(struct Window *view) {
    if (view->wlr_handle != NULL || server.foreign_toplevel_manager == NULL)
        return;

    view->wlr_handle =
        wlr_foreign_toplevel_handle_v1_create(server.foreign_toplevel_manager);

    if (view->wlr_handle == NULL) return;

    wlr_foreign_toplevel_handle_v1_set_title(
        view->wlr_handle,
        view->xdg_toplevel->title ? view->xdg_toplevel->title : ""
    );

    wlr_foreign_toplevel_handle_v1_set_app_id(
        view->wlr_handle,
        view->xdg_toplevel->app_id ? view->xdg_toplevel->app_id : ""
    );

    struct Output *o = output_primary();
    if (o != NULL) {
        wlr_foreign_toplevel_handle_v1_output_enter(
            view->wlr_handle, o->wlr_output
        );
    }

    view->wlr_request_activate.notify = wlr_handle_request_activate;
    wl_signal_add(
        &view->wlr_handle->events.request_activate, &view->wlr_request_activate
    );

    view->wlr_request_close.notify = wlr_handle_request_close;
    wl_signal_add(
        &view->wlr_handle->events.request_close, &view->wlr_request_close
    );
}

static void wlr_handle_destroy(struct Window *view) {
    if (view->wlr_handle == NULL) return;

    wl_list_remove(&view->wlr_request_activate.link);
    wl_list_remove(&view->wlr_request_close.link);
    wlr_foreign_toplevel_handle_v1_destroy(view->wlr_handle);

    view->wlr_handle = NULL;
}

static void foreign_handle_create(struct Window *view) {
    if (view->foreign_handle != NULL || server.foreign_toplevel_list == NULL)
        return;

    const struct wlr_ext_foreign_toplevel_handle_v1_state state = {
        .title = view->xdg_toplevel->title ? view->xdg_toplevel->title : "",
        .app_id = view->xdg_toplevel->app_id ? view->xdg_toplevel->app_id : "",
    };

    view->foreign_handle = wlr_ext_foreign_toplevel_handle_v1_create(
        server.foreign_toplevel_list, &state
    );
}

static void foreign_handle_destroy(struct Window *view) {
    if (view->foreign_handle == NULL) return;
    wlr_ext_foreign_toplevel_handle_v1_destroy(view->foreign_handle);
    view->foreign_handle = NULL;
}

static void foreign_handle_update(struct Window *view) {
    if (view->foreign_handle == NULL) return;

    const struct wlr_ext_foreign_toplevel_handle_v1_state state = {
        .title = view->xdg_toplevel->title ? view->xdg_toplevel->title : "",
        .app_id = view->xdg_toplevel->app_id ? view->xdg_toplevel->app_id : "",
    };

    wlr_ext_foreign_toplevel_handle_v1_update_state(
        view->foreign_handle, &state
    );
}

static void view_handle_map(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *view = wl_container_of(l, view, map);

    // The map signal and `view_handle_commit()` both call us on the same
    // commit—the first one gets the cake.
    if (view->mapped) return;
    view->mapped = true;

    wlr_handle_create(view);
    foreign_handle_create(view);
    wm_mark_layout_dirty();
    wlr_scene_node_set_enabled(&view->scene_tree->node, true);

    view_set_tiled(view, layouts_is_tiled(wm.layout));

    // Update the focus stack, grant keyboard focus to the client (sole
    // `wlr_seat_keyboard_notify_enter` caller).
    seat_focus(view);

    ipc_notify_window_opened(view);
    idle_update_inhibit();
}

static void view_handle_unmap(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *view = wl_container_of(l, view, unmap);

    view->mapped = false;

    wlr_handle_destroy(view);
    foreign_handle_destroy(view);
    wm_mark_layout_dirty();
    wlr_scene_node_set_enabled(&view->scene_tree->node, false);

    ipc_notify_window_closed(view);
    idle_update_inhibit();
}

static void view_handle_commit(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *view = wl_container_of(l, view, commit);
    struct wlr_xdg_surface *surf = view->xdg_toplevel->base;

    if (!view->initial_configure_sent) {
        view->initial_configure_sent = true;
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
        return;
    }

    if (!surf->initialized) return;

    int32_t w = surf->geometry.width;
    int32_t h = surf->geometry.height;

    if (w > 0 && h > 0) {
        view->width = w;
        view->height = h;
    }

    if (!view->mapped && surf->surface->mapped) {
        view_handle_map(&view->map, NULL);
    }
}

static void view_handle_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *view = wl_container_of(l, view, destroy);
    view_destroy(view);
}

static void view_handle_request_move(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *view = wl_container_of(l, view, request_move);
    if (!layouts_is_tiled(wm.layout)) {
        cursor_begin_interactive(view, SEAT_OP_MOVE, 0);
    }
}

static void view_handle_request_resize(struct wl_listener *l, void *data) {
    struct wlr_xdg_toplevel_resize_event *ev = data;
    struct Window *view = wl_container_of(l, view, request_resize);

    if (!layouts_is_tiled(wm.layout)) {
        cursor_begin_interactive(view, SEAT_OP_RESIZE, ev->edges);
    }
}

static void view_handle_request_maximize(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *view = wl_container_of(l, view, request_maximize);
    struct wlr_xdg_surface *surf = view->xdg_toplevel->base;

    if (surf->initialized) { wlr_xdg_surface_schedule_configure(surf); }
}

static void view_handle_request_fullscreen(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *view = wl_container_of(l, view, request_fullscreen);
    if (!view->xdg_toplevel->base->initialized) return;

    view_set_fullscreen(view, view->xdg_toplevel->requested.fullscreen);
}

static void view_handle_request_minimize(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *view = wl_container_of(l, view, request_minimize);
    (void)view; // no "minimize" for now
}

static void view_handle_set_title(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *view = wl_container_of(l, view, set_title);
    const char *new_title =
        view->xdg_toplevel->title ? view->xdg_toplevel->title : "";

    const char *old_title = view->title ? view->title : "";
    if (strcmp(new_title, old_title) == 0) return;

    free(view->title);
    view->title = strdup(new_title);

    if (view->wlr_handle)
        wlr_foreign_toplevel_handle_v1_set_title(view->wlr_handle, new_title);

    foreign_handle_update(view);
    ipc_notify_window_meta(view);
}

static void view_handle_set_app_id(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *view = wl_container_of(l, view, set_app_id);
    const char *new_id =
        view->xdg_toplevel->app_id ? view->xdg_toplevel->app_id : "";

    const char *old_id = view->app_id ? view->app_id : "";
    if (strcmp(new_id, old_id) == 0) return;

    free(view->app_id);
    view->app_id = strdup(new_id);

    if (view->wlr_handle)
        wlr_foreign_toplevel_handle_v1_set_app_id(view->wlr_handle, new_id);

    foreign_handle_update(view);
    ipc_notify_window_meta(view);
}

void view_create(struct wlr_xdg_toplevel *xdg_toplevel) {
    struct Window *view = calloc(1, sizeof(*view));
    if (view == NULL) return;

    view->xdg_toplevel = xdg_toplevel;
    view->space = wm.current_space;
    view->node_tag.type = NODE_WINDOW;
    view->scene_tree->node.data = &view->node_tag;

    struct wlr_scene_tree *parent = layer_for_layout(wm.layout);
    view->scene_tree = wlr_scene_xdg_surface_create(parent, xdg_toplevel->base);
    if (view->scene_tree == NULL) {
        free(view);
        return;
    }

    xdg_toplevel->base->data = view->scene_tree;
    view->popup_tree = view->scene_tree;
    view->scene_tree->node.data = view;

    wlr_scene_node_set_enabled(&view->scene_tree->node, false);

    view->map.notify = view_handle_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &view->map);
    view->unmap.notify = view_handle_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &view->unmap);
    view->commit.notify = view_handle_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &view->commit);
    view->destroy.notify = view_handle_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &view->destroy);
    view->request_move.notify = view_handle_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &view->request_move);
    view->request_resize.notify = view_handle_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &view->request_resize);
    view->request_maximize.notify = view_handle_request_maximize;
    wl_signal_add(
        &xdg_toplevel->events.request_maximize, &view->request_maximize
    );

    view->request_fullscreen.notify = view_handle_request_fullscreen;
    wl_signal_add(
        &xdg_toplevel->events.request_fullscreen, &view->request_fullscreen
    );

    view->request_minimize.notify = view_handle_request_minimize;
    wl_signal_add(
        &xdg_toplevel->events.request_minimize, &view->request_minimize
    );

    view->set_title.notify = view_handle_set_title;
    wl_signal_add(&xdg_toplevel->events.set_title, &view->set_title);
    view->set_app_id.notify = view_handle_set_app_id;
    wl_signal_add(&xdg_toplevel->events.set_app_id, &view->set_app_id);

    wl_list_init(&view->focus_link);
    wl_list_insert(wm.windows.prev, &view->link);
}

void view_destroy(struct Window *view) {
    if (view->closed) return;
    view->closed = true;

    wl_list_remove(&view->focus_link);

    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->request_maximize.link);
    wl_list_remove(&view->request_fullscreen.link);
    wl_list_remove(&view->request_minimize.link);
    wl_list_remove(&view->set_title.link);
    wl_list_remove(&view->set_app_id.link);

    if (view->scene_tree != NULL) {
        wlr_scene_node_destroy(&view->scene_tree->node);
        view->scene_tree = NULL;
    }

    wlr_handle_destroy(view);
    ipc_notify_window_closed(view);
    wl_list_remove(&view->link);
    wm_mark_layout_dirty();
    foreign_handle_destroy(view);
    idle_update_inhibit();

    free(view->title);
    free(view->app_id);
    free(view);
}

struct Window *view_focused(void) {
    struct Window *w;
    wl_list_for_each_reverse(w, &wm.focus_stack, focus_link) {
        if (w->closed || w->space_hidden) continue;
        return w;
    }

    return NULL;
}

struct Window *view_at(
    double lx, double ly, struct wlr_surface **surface, double *sx, double *sy
) {
    double nx = lx, ny = ly;
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server.scene->tree.node, nx, ny, &nx, &ny);

    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) return NULL;

    struct wlr_scene_buffer *buf = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *ssurf = wlr_scene_surface_try_from_buffer(buf);
    if (ssurf == NULL) return NULL;

    *surface = ssurf->surface;
    *sx = nx;
    *sy = ny;

    return window_from_scene_node(node);
}

void view_set_position(struct Window *view, int32_t x, int32_t y) {
    if (view->pos_valid && view->x == x && view->y == y) return;
    wlr_scene_node_set_position(&view->scene_tree->node, x, y);
    view->x = x;
    view->y = y;
    view->pos_valid = true;
}

void view_set_size(struct Window *view, int32_t w, int32_t h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    struct wlr_xdg_surface *surf = view->xdg_toplevel->base;
    if (!surf->initialized) { return; }

    if (view->prop_valid && view->prop_w == w && view->prop_h == h) return;
    wlr_xdg_toplevel_set_size(view->xdg_toplevel, w, h);
    view->prop_w = w;
    view->prop_h = h;
    view->prop_valid = true;
}

void view_set_tiled(struct Window *view, bool tiled) {
    struct wlr_xdg_surface *surf = view->xdg_toplevel->base;
    if (!surf->initialized) return;

    if (tiled) {
        wlr_xdg_toplevel_set_tiled(
            view->xdg_toplevel,
            WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT
        );
    } else {
        wlr_xdg_toplevel_set_tiled(view->xdg_toplevel, WLR_EDGE_NONE);
    }
}

void view_set_clip(struct Window *view, int32_t w, int32_t h) {
    struct wlr_box geo = view->xdg_toplevel->base->geometry;
    struct wlr_box clip = {.x = geo.x, .y = geo.y, .width = w, .height = h};
    wlr_scene_subsurface_tree_set_clip(&view->scene_tree->node, &clip);
}

void view_clear_clip(struct Window *view) {
    wlr_scene_subsurface_tree_set_clip(&view->scene_tree->node, NULL);
}

void view_close(struct Window *view) {
    wlr_xdg_toplevel_send_close(view->xdg_toplevel);
}

void view_raise(struct Window *view) {
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
}

void view_reparent(struct Window *view, bool to_float) {
    struct wlr_scene_tree *target =
        view->fullscreen ? server.fullscreen_tree
                         : (to_float ? server.float_tree : server.tile_tree);

    if (view->scene_tree->node.parent == target) return;
    wlr_scene_node_reparent(&view->scene_tree->node, target);
}
