#include "window.h"

#include "animation.h"
#include "config.h"
#include "cursor.h"
#include "ipc.h"
#include "layouts/layout.h"
#include "log.h"
#include "manager.h"
#include "output.h"
#include "rules.h"
#include "seat.h"
#include "server.h"

#include <nullspace/layouts.h>
#include <scenefx/types/wlr_scene.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#define LOG_TOPIC "window"

static struct wlr_scene_tree *layer_for_layout(enum Layout layout) {
    return layouts_is_tiled(layout) ? server.tile_tree : server.float_tree;
}

static struct wlr_scene_buffer *find_surface_buffer(
    struct wlr_scene_node *node, const struct wlr_surface *surface
) {
    if (node == NULL || surface == NULL) return NULL;

    if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *buf = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *ssurf =
            wlr_scene_surface_try_from_buffer(buf);

        return (ssurf != NULL && ssurf->surface == surface) ? buf : NULL;
    }

    if (node->type == WLR_SCENE_NODE_TREE) {
        struct wlr_scene_node *child;
        wl_list_for_each(
            child, &wlr_scene_tree_from_node(node)->children, link
        ) {
            struct wlr_scene_buffer *found =
                find_surface_buffer(child, surface);

            if (found != NULL) return found;
        }
    }

    return NULL;
}

// TODO rewire to allow state changes (via IPC)
static void window_update_corners(struct Window *window) {
    if (window == NULL || window->closed || window->scene_tree == NULL) return;

    struct wlr_surface *surface = window->xdg_toplevel->base->surface;
    struct wlr_scene_buffer *buf =
        find_surface_buffer(&window->scene_tree->node, surface);

    if (buf == NULL) return;

    int radius = CFG_ROUNDED_CORNERS_RADIUS;
    // lets cap the range for now
    if (radius <= 0) radius = 2;

    bool round = CFG_ROUNDED_CORNERS_ENABLED && !window->fullscreen;
    wlr_scene_buffer_set_corner_radius(buf, round ? radius : 0);
}

void window_apply_target(
    struct Window *w, int32_t nx, int32_t ny, int32_t nw, int32_t nh
) {
    if (w->space_hidden) return;
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;

    // Consume the reveal flag unconditionally
    int32_t reveal_dir = w->reveal_dir;
    w->reveal_dir = 0;

    struct AnimationBox target = animation_box(nx, ny, nw, nh);
    if (w->has_target && animation_box_eq(&w->target_box, &target)) return;

    bool first = !w->has_target;
    w->target_box = target;
    w->has_target = true;

    if (!wm.animations.enabled) {
        w->anim.running = false;
        w->anim.current = target;
        window_set_position(w, nx, ny);
        window_set_size(w, nw, nh);
        window_clear_clip(w);
        return;
    }

    struct AnimationBox from;
    enum AnimationType type;
    int32_t duration;

    if (first && (reveal_dir != 0 || !w->pos_valid)) {
        if (reveal_dir != 0) {
            // Horizontal space slide across the whole output.
            struct Output *out = output_primary();
            int32_t offset = (out != NULL && out->width > 0) ? out->width : nw;
            if (offset < 1) offset = 1;
            from = target;
            from.x = target.x + reveal_dir * offset;
            type = ANIM_SPACE;
            duration = wm.animations.duration_space;
        } else {
            // Slide from above/below
            from = target;
            from.y = wm.animations.open_from_top ? -target.height
                                                 : target.y + target.height;

            type = ANIM_OPEN;
            duration = wm.animations.duration_open;
        }

        window_set_position(w, from.x, from.y);
        window_set_size(w, target.width, target.height);
    } else {
        if (w->anim.running) {
            from = w->anim.current;
        } else if (w->pos_valid && w->width > 0 && w->height > 0) {
            from = animation_box(w->x, w->y, w->width, w->height);
        } else if (w->pos_valid && w->prop_valid) {
            from = animation_box(w->x, w->y, w->prop_w, w->prop_h);
        } else {
            from = target;
        }

        type = ANIM_MOVE;
        duration = wm.animations.duration_move;
    }

    animation_start(&w->anim, type, &from, &target, duration);
    window_set_position(w, w->anim.current.x, w->anim.current.y);
    window_set_size(w, target.width, target.height);

    if (w->anim.running && type == ANIM_MOVE) {
        window_set_clip(w, w->anim.current.width, w->anim.current.height);
    } else {
        window_clear_clip(w);
    }
}

void window_set_activated(struct Window *window, bool activated) {
    wlr_xdg_toplevel_set_activated(window->xdg_toplevel, activated);
    if (window->wlr_handle != NULL) {
        wlr_foreign_toplevel_handle_v1_set_activated(
            window->wlr_handle, activated
        );
    }
}

void window_set_fullscreen(struct Window *window, bool fullscreen) {
    if (window->closed || window->fullscreen == fullscreen) return;
    if (!window->xdg_toplevel->base->initialized) return;

    if (fullscreen) {
        window->fs_x = window->x;
        window->fs_y = window->y;
        window->fs_w = window->width;
        window->fs_h = window->height;
    }

    window->fullscreen = fullscreen;
    wlr_xdg_toplevel_set_fullscreen(window->xdg_toplevel, fullscreen);
    window_update_corners(window);

    window->has_target = false;
    window_reparent(window, !layouts_is_tiled(wm.layout));

    if (!fullscreen && !layouts_is_tiled(wm.layout)) { // floating
        // restore from saved
        window_apply_target(
            window, window->fs_x, window->fs_y, window->fs_w, window->fs_h
        );
    }

    if (fullscreen) window_raise(window);
    wm_mark_layout_dirty();
}

static void wlr_handle_request_activate(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, wlr_request_activate);
    if (window->closed) return;
    if (window->space_hidden) wm_switch_space(window->space);

    seat_focus(window);
}

static void wlr_handle_request_close(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, wlr_request_close);
    window_close(window);
}

static void wlr_handle_create(struct Window *window) {
    if (window->wlr_handle != NULL || server.foreign_toplevel_manager == NULL)
        return;

    window->wlr_handle =
        wlr_foreign_toplevel_handle_v1_create(server.foreign_toplevel_manager);

    if (window->wlr_handle == NULL) return;

    wlr_foreign_toplevel_handle_v1_set_title(
        window->wlr_handle,
        window->xdg_toplevel->title ? window->xdg_toplevel->title : ""
    );

    wlr_foreign_toplevel_handle_v1_set_app_id(
        window->wlr_handle,
        window->xdg_toplevel->app_id ? window->xdg_toplevel->app_id : ""
    );

    struct Output *o = output_primary();
    if (o != NULL) {
        wlr_foreign_toplevel_handle_v1_output_enter(
            window->wlr_handle, o->wlr_output
        );
    }

    window->wlr_request_activate.notify = wlr_handle_request_activate;
    wl_signal_add(
        &window->wlr_handle->events.request_activate,
        &window->wlr_request_activate
    );

    window->wlr_request_close.notify = wlr_handle_request_close;
    wl_signal_add(
        &window->wlr_handle->events.request_close, &window->wlr_request_close
    );
}

static void wlr_handle_destroy(struct Window *window) {
    if (window->wlr_handle == NULL) return;

    wl_list_remove(&window->wlr_request_activate.link);
    wl_list_remove(&window->wlr_request_close.link);
    wlr_foreign_toplevel_handle_v1_destroy(window->wlr_handle);

    window->wlr_handle = NULL;
}

static void foreign_handle_create(struct Window *window) {
    if (window->foreign_handle != NULL || server.foreign_toplevel_list == NULL)
        return;

    const struct wlr_ext_foreign_toplevel_handle_v1_state state = {
        .title = window->xdg_toplevel->title ? window->xdg_toplevel->title : "",
        .app_id =
            window->xdg_toplevel->app_id ? window->xdg_toplevel->app_id : "",
    };

    window->foreign_handle = wlr_ext_foreign_toplevel_handle_v1_create(
        server.foreign_toplevel_list, &state
    );
}

static void foreign_handle_destroy(struct Window *window) {
    if (window->foreign_handle == NULL) return;
    wlr_ext_foreign_toplevel_handle_v1_destroy(window->foreign_handle);
    window->foreign_handle = NULL;
}

static void foreign_handle_update(struct Window *window) {
    if (window->foreign_handle == NULL) return;

    const struct wlr_ext_foreign_toplevel_handle_v1_state state = {
        .title = window->xdg_toplevel->title ? window->xdg_toplevel->title : "",
        .app_id =
            window->xdg_toplevel->app_id ? window->xdg_toplevel->app_id : "",
    };

    wlr_ext_foreign_toplevel_handle_v1_update_state(
        window->foreign_handle, &state
    );
}

static void window_apply_rules(struct Window *window) {
    if (window == NULL || window->closed || window->rules_applied) return;

    const char *app_id = window->xdg_toplevel->app_id;
    const char *title = window->xdg_toplevel->title;

    window->rules_applied = true;

    struct WindowRuleActions act;
    if (!rules_match(app_id, title, &act)) return;

    if (act.has_space) wm_assign_new_window_space(window, act.space);
}

static void window_handle_map(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, map);

    // The map signal and `window_handle_commit()` both call us on the same
    // commit—the first one gets the cake.
    if (window->mapped) return;
    window->mapped = true;

    window_apply_rules(window);
    wlr_handle_create(window);
    foreign_handle_create(window);
    wm_mark_layout_dirty();
    window_update_corners(window);
    wlr_scene_node_set_enabled(&window->scene_tree->node, true);
    wlr_scene_node_set_enabled(&window->popup_tree->node, true);

    window_set_tiled(window, layouts_is_tiled(wm.layout));

    if (window->space_hidden) {
        wl_list_remove(&window->focus_link);
        wl_list_insert(&wm.focus_stack, &window->focus_link);
    } else {
        // Update the focus stack, grant keyboard focus to the client (sole
        // `wlr_seat_keyboard_notify_enter` caller).
        seat_focus(window);
    }

    ipc_notify_window_opened(window);
    idle_update_inhibit();
}

static void window_handle_unmap(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, unmap);

    window->mapped = false;

    wlr_handle_destroy(window);
    foreign_handle_destroy(window);
    wm_mark_layout_dirty();
    wlr_scene_node_set_enabled(&window->scene_tree->node, false);
    wlr_scene_node_set_enabled(&window->popup_tree->node, false);

    ipc_notify_window_closed(window);
    idle_update_inhibit();
}

static void window_handle_commit(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, commit);
    struct wlr_xdg_surface *surf = window->xdg_toplevel->base;

    if (!window->initial_configure_sent) {
        window->initial_configure_sent = true;
        wlr_xdg_toplevel_set_size(window->xdg_toplevel, 0, 0);
        return;
    }

    if (!surf->initialized) return;

    int32_t w = surf->geometry.width;
    int32_t h = surf->geometry.height;

    if (w > 0 && h > 0) {
        window->width = w;
        window->height = h;
    }

    if (!window->mapped && surf->surface->mapped) {
        window_handle_map(&window->map, NULL);
    }
}

static void window_handle_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, destroy);
    window_destroy(window);
}

static void window_handle_request_move(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, request_move);
    if (!layouts_is_tiled(wm.layout)) {
        cursor_begin_interactive(window, SEAT_OP_MOVE, 0);
    }
}

static void window_handle_request_resize(struct wl_listener *l, void *data) {
    struct wlr_xdg_toplevel_resize_event *ev = data;
    struct Window *window = wl_container_of(l, window, request_resize);

    if (!layouts_is_tiled(wm.layout)) {
        cursor_begin_interactive(window, SEAT_OP_RESIZE, ev->edges);
    }
}

static void window_handle_request_maximize(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, request_maximize);
    struct wlr_xdg_surface *surf = window->xdg_toplevel->base;

    if (surf->initialized) { wlr_xdg_surface_schedule_configure(surf); }
}

static void
window_handle_request_fullscreen(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, request_fullscreen);
    if (!window->xdg_toplevel->base->initialized) return;

    window_set_fullscreen(window, window->xdg_toplevel->requested.fullscreen);
}

static void window_handle_request_minimize(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, request_minimize);
    (void)window; // no "minimize" for now
}

static void window_handle_set_title(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, set_title);
    const char *new_title =
        window->xdg_toplevel->title ? window->xdg_toplevel->title : "";

    const char *old_title = window->title ? window->title : "";
    if (strcmp(new_title, old_title) == 0) return;

    char *title_copy = strdup(new_title);
    if (title_copy == NULL) {
        log_err("unable to cache window title: OOM");
        return;
    }

    free(window->title);
    window->title = title_copy;

    if (window->wlr_handle)
        wlr_foreign_toplevel_handle_v1_set_title(window->wlr_handle, new_title);

    foreign_handle_update(window);
    ipc_notify_window_meta(window);
}

static void window_handle_set_app_id(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, set_app_id);
    const char *new_id =
        window->xdg_toplevel->app_id ? window->xdg_toplevel->app_id : "";

    const char *old_id = window->app_id ? window->app_id : "";
    if (strcmp(new_id, old_id) == 0) return;

    char *app_id_copy = strdup(new_id);
    if (app_id_copy == NULL) {
        log_err("unable to cache window app_id: OOM");
        return;
    }

    free(window->app_id);
    window->app_id = app_id_copy;

    if (window->wlr_handle)
        wlr_foreign_toplevel_handle_v1_set_app_id(window->wlr_handle, new_id);

    foreign_handle_update(window);
    ipc_notify_window_meta(window);
}

struct Window *window_from_scene_node(struct wlr_scene_node *node) {
    struct NodeTag *tag = node_tag_find(node);
    if (tag == NULL || tag->type != NODE_WINDOW) return NULL;

    struct Window *w = wl_container_of(tag, w, node_tag);
    return w;
}

void window_create(struct wlr_xdg_toplevel *xdg_toplevel) {
    struct Window *window = calloc(1, sizeof(*window));
    if (window == NULL) return;

    window->xdg_toplevel = xdg_toplevel;
    window->space = wm.current_space;
    window->node_tag.type = NODE_WINDOW;

    struct wlr_scene_tree *parent = layer_for_layout(wm.layout);
    window->scene_tree =
        wlr_scene_xdg_surface_create(parent, xdg_toplevel->base);

    if (window->scene_tree == NULL) {
        free(window);
        return;
    }

    xdg_toplevel->base->data = window->scene_tree;
    window->scene_tree->node.data = &window->node_tag;
    window->popup_tree = wlr_scene_tree_create(server.popup_tree);

    if (window->popup_tree == NULL) {
        wlr_scene_node_destroy(&window->scene_tree->node);
        free(window);
        return;
    }

    window->popup_tree->node.data = &window->node_tag;

    window_update_corners(window);
    wlr_scene_node_set_enabled(&window->scene_tree->node, false);
    wlr_scene_node_set_enabled(&window->popup_tree->node, false);

    window->map.notify = window_handle_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &window->map);
    window->unmap.notify = window_handle_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &window->unmap);
    window->commit.notify = window_handle_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &window->commit);
    window->destroy.notify = window_handle_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &window->destroy);
    window->request_move.notify = window_handle_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &window->request_move);
    window->request_resize.notify = window_handle_request_resize;
    wl_signal_add(
        &xdg_toplevel->events.request_resize, &window->request_resize
    );

    window->request_maximize.notify = window_handle_request_maximize;
    wl_signal_add(
        &xdg_toplevel->events.request_maximize, &window->request_maximize
    );

    window->request_fullscreen.notify = window_handle_request_fullscreen;
    wl_signal_add(
        &xdg_toplevel->events.request_fullscreen, &window->request_fullscreen
    );

    window->request_minimize.notify = window_handle_request_minimize;
    wl_signal_add(
        &xdg_toplevel->events.request_minimize, &window->request_minimize
    );

    window->set_title.notify = window_handle_set_title;
    wl_signal_add(&xdg_toplevel->events.set_title, &window->set_title);
    window->set_app_id.notify = window_handle_set_app_id;
    wl_signal_add(&xdg_toplevel->events.set_app_id, &window->set_app_id);

    wl_list_init(&window->focus_link);
    wl_list_insert(wm.windows.prev, &window->link);
}

void window_destroy(struct Window *window) {
    if (window->closed) return;
    window->closed = true;
    cursor_window_destroyed(window);

    wl_list_remove(&window->focus_link);

    wl_list_remove(&window->map.link);
    wl_list_remove(&window->unmap.link);
    wl_list_remove(&window->commit.link);
    wl_list_remove(&window->destroy.link);
    wl_list_remove(&window->request_move.link);
    wl_list_remove(&window->request_resize.link);
    wl_list_remove(&window->request_maximize.link);
    wl_list_remove(&window->request_fullscreen.link);
    wl_list_remove(&window->request_minimize.link);
    wl_list_remove(&window->set_title.link);
    wl_list_remove(&window->set_app_id.link);

    if (window->scene_tree != NULL) {
        wlr_scene_node_destroy(&window->scene_tree->node);
        window->scene_tree = NULL;
    }

    if (window->popup_tree != NULL) {
        wlr_scene_node_destroy(&window->popup_tree->node);
        window->popup_tree = NULL;
    }

    wlr_handle_destroy(window);
    ipc_notify_window_closed(window);
    wl_list_remove(&window->link);
    wm_mark_layout_dirty();
    foreign_handle_destroy(window);
    idle_update_inhibit();

    free(window->title);
    free(window->app_id);
    free(window);
}

struct Window *window_focused(void) {
    struct Window *w;
    wl_list_for_each_reverse(w, &wm.focus_stack, focus_link) {
        if (w->closed || w->space_hidden) continue;
        return w;
    }

    return NULL;
}

struct Window *window_at(
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

void window_set_position(struct Window *window, int32_t x, int32_t y) {
    if (window->pos_valid && window->x == x && window->y == y) return;

    wlr_scene_node_set_position(&window->scene_tree->node, x, y);
    wlr_scene_node_set_position(&window->popup_tree->node, x, y);

    window->x = x;
    window->y = y;
    window->pos_valid = true;
}

void window_set_size(struct Window *window, int32_t w, int32_t h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    struct wlr_xdg_surface *surf = window->xdg_toplevel->base;
    if (!surf->initialized) { return; }

    if (window->prop_valid && window->prop_w == w && window->prop_h == h)
        return;

    wlr_xdg_toplevel_set_size(window->xdg_toplevel, w, h);
    window->prop_w = w;
    window->prop_h = h;
    window->prop_valid = true;
}

void window_set_tiled(struct Window *window, bool tiled) {
    struct wlr_xdg_surface *surf = window->xdg_toplevel->base;
    if (!surf->initialized) return;

    if (tiled) {
        wlr_xdg_toplevel_set_tiled(
            window->xdg_toplevel,
            WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT
        );
    } else {
        wlr_xdg_toplevel_set_tiled(window->xdg_toplevel, WLR_EDGE_NONE);
    }
}

void window_set_clip(struct Window *window, int32_t w, int32_t h) {
    struct wlr_box geo = window->xdg_toplevel->base->geometry;
    struct wlr_box clip = {.x = geo.x, .y = geo.y, .width = w, .height = h};

    wlr_scene_subsurface_tree_set_clip(&window->scene_tree->node, &clip);
}

void window_clear_clip(struct Window *window) {
    wlr_scene_subsurface_tree_set_clip(&window->scene_tree->node, NULL);
}

void window_close(struct Window *window) {
    wlr_xdg_toplevel_send_close(window->xdg_toplevel);
}

void window_raise(struct Window *window) {
    wlr_scene_node_raise_to_top(&window->scene_tree->node);

    if (window->popup_tree != NULL)
        wlr_scene_node_raise_to_top(&window->popup_tree->node);
}

void window_reparent(struct Window *window, bool to_float) {
    struct wlr_scene_tree *target =
        window->fullscreen ? server.fullscreen_tree
                           : (to_float ? server.float_tree : server.tile_tree);

    if (window->scene_tree->node.parent == target) return;
    wlr_scene_node_reparent(&window->scene_tree->node, target);
}
