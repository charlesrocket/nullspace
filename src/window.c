#include "window.h"

#include "animation.h"
#include "cursor.h"
#include "decoration.h"
#include "icon.h"
#include "ipc.h"
#include "layout/layout.h"
#include "log.h"
#include "manager.h"
#include "output.h"
#include "rules.h"
#include "seat.h"
#include "server.h"

#include <nullspace/layouts.h>
#include <scenefx/types/fx/clipped_region.h>
#include <scenefx/types/wlr_scene.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#ifdef XWAYLAND
#include <wlr/xwayland.h>
#endif

#define LOG_TOPIC "window"

#ifdef XWAYLAND
static void xw_unbind(struct Window *window);
#endif

static int imin(int a, int b) { return a < b ? a : b; }

static struct wlr_scene_tree *layer_for_layout(enum Layout layout) {
    return layouts_is_tiled(layout) ? server.tile_tree : server.float_tree;
}

bool window_usable_area(const struct Window *w, struct wlr_box *out) {
    if (w == NULL || out == NULL) return false;

    const struct Output *o = w->output;
    if (o == NULL) o = output_primary();
    if (o == NULL) return false;

    if (o->area_set && o->area_width > 0 && o->area_height > 0) {
        *out = (struct wlr_box){
            .x = o->area_x,
            .y = o->area_y,
            .width = o->area_width,
            .height = o->area_height,
        };

        return true;
    }

    if (o->width <= 0 || o->height <= 0) return false;

    *out = (struct wlr_box){
        .x = o->pos_x,
        .y = o->pos_y,
        .width = o->width,
        .height = o->height,
    };

    return true;
}

int32_t window_visual_top_offset(const struct Window *w) {
    if (w == NULL) return 0;
    if (w->closed || w->decoration == NULL || w->decoration->tab == NULL) {
        return 0;
    }

    return DECO_TAB_H;
}

void window_clamp_to_usable(
    struct Window *w, int32_t *x, int32_t *y, int32_t *width, int32_t *height
) {
    if (x == NULL || y == NULL || width == NULL || height == NULL) return;

    struct wlr_box ua;
    if (!window_usable_area(w, &ua)) return;
    if (ua.width <= 0 || ua.height <= 0) return;

    const int32_t deco_top = window_visual_top_offset(w);
    int32_t max_h = ua.height - deco_top;
    if (max_h < 1) max_h = 1;

    if (*width > ua.width) *width = ua.width;
    if (*height > max_h) *height = max_h;

    if (*x + *width > ua.x + ua.width) *x = ua.x + ua.width - *width;
    if (*y + *height > ua.y + ua.height) *y = ua.y + ua.height - *height;
    if (*x < ua.x) *x = ua.x;

    const int32_t min_y = ua.y + deco_top;
    if (*y < min_y) *y = min_y;
}

struct CornerApply {
    const struct wlr_surface *root;
    struct fx_corner_radii radii;
    int applied;
};

static bool surface_is_under(
    const struct wlr_surface *surface, const struct wlr_surface *root
) {
    while (surface != NULL) {
        if (surface == root) return true;

        struct wlr_subsurface *sub =
            wlr_subsurface_try_from_wlr_surface((struct wlr_surface *)surface);

        if (sub == NULL) break;

        surface = sub->parent;
    }

    return false;
}

static void
apply_radius(struct wlr_scene_buffer *buf, int sx, int sy, void *data) {
    (void)sx;
    (void)sy;

    struct CornerApply *ctx = data;
    struct wlr_scene_surface *sc_surf = wlr_scene_surface_try_from_buffer(buf);
    if (sc_surf == NULL) return;
    if (!surface_is_under(sc_surf->surface, ctx->root)) return;

    wlr_scene_buffer_set_corner_radii(buf, ctx->radii);
    ctx->applied++;
}

static int corner_radius_for(const struct Window *window) {
    if (!wm.rounded_corners || window->fullscreen) return 0;

    int r = wm.rounded_corners_radius;
    if (r < 0) r = 0;
    if (r > UINT16_MAX) r = UINT16_MAX;

    // Corners on the same edge must never overlap.
    if (window->width > 0 && window->height > 0) {
        r = imin(r, imin(window->width, window->height) / 2);
    }

    return r;
}

// TODO rewire to allow state changes (via IPC)
void window_update_corners(struct Window *window) {
    if (window == NULL || window->closed || window->scene_tree == NULL) return;

    struct wlr_surface *root = window_wlr_surface(window);
    if (root == NULL) return;

    int r = corner_radius_for(window);

    int tl = r, tr = r, bl = r, br = r;

    const struct Decoration *deco = window->decoration;
    if (r > 0 && deco != NULL && deco->tab != NULL) {
        int win_w = window->width > 0 ? window->width : 1;

        int left_gap = deco->tab_x;
        int right_gap = win_w - (deco->tab_x + deco->tab_w);
        if (left_gap < 0) left_gap = 0;
        if (right_gap < 0) right_gap = 0;

        tl = imin(tl, left_gap);
        tr = imin(tr, right_gap);
    }

    if (window->last_tl == tl && window->last_tr == tr && window->last_bl == bl
        && window->last_br == br) {
        return;
    }

    struct CornerApply ctx = {
        .root = root,
        .radii =
            {
                    .top_left = (uint16_t)tl,
                    .top_right = (uint16_t)tr,
                    .bottom_left = (uint16_t)bl,
                    .bottom_right = (uint16_t)br,
                    },
        .applied = 0,
    };

    wlr_scene_node_for_each_buffer(
        &window->scene_tree->node, apply_radius, &ctx
    );

    if (ctx.applied == 0) return;

    window->last_tl = tl;
    window->last_tr = tr;
    window->last_bl = bl;
    window->last_br = br;

    deco_update_blur(window);
}

// May return NULL for X11 windows whose wl_surface is not associated yet.
struct wlr_surface *window_wlr_surface(const struct Window *window) {
#ifdef XWAYLAND
    if (window->is_xwayland) { return window->xwayland_surface->surface; }
#endif
    return window->xdg_toplevel->base->surface;
}

const char *window_title(const struct Window *window) {
    return window->title ? window->title : "";
}

const char *window_app_id(const struct Window *window) {
    return window->app_id ? window->app_id : "";
}

void window_apply_target(
    struct Window *w, int32_t nx, int32_t ny, int32_t nw, int32_t nh
) {
    if (w->space_hidden) return;
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (!w->fullscreen) { window_clamp_to_usable(w, &nx, &ny, &nw, &nh); }

    // Consume the reveal flag unconditionally
    int32_t reveal_dir = w->reveal_dir;
    w->reveal_dir = 0;

    struct AnimationBox target = animation_box(nx, ny, nw, nh);
    if (w->has_target && animation_box_eq(&w->target_box, &target)) return;

    bool first = !w->has_target;
    w->target_box = target;
    w->has_target = true;

    if (!wm.animations.enabled || wm.resizing) {
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
            struct Output *out = w->output;
            if (out == NULL) out = output_primary();
            int32_t offset = (out != NULL && out->width > 0) ? out->width : nw;
            if (offset < 1) offset = 1;
            from = target;
            from.x = target.x + reveal_dir * offset;
            type = ANIM_SPACE;
            duration = wm.animations.duration_space;
        } else { // slide from above/below
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
#ifdef XWAYLAND
        } else if (w->is_xwayland && w->xwayland_surface
                   && w->xwayland_surface->width > 0) {
            from = animation_box(
                w->x, w->y, w->xwayland_surface->width,
                w->xwayland_surface->height
            );
#endif
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
#ifdef XWAYLAND
    if (window->is_xwayland) {
        wlr_xwayland_surface_activate(window->xwayland_surface, activated);
    } else
#endif
    {
        if (window->xdg_toplevel->base->initialized) {
            wlr_xdg_toplevel_set_activated(window->xdg_toplevel, activated);
        }
    }

    if (window->wlr_handle != NULL) {
        wlr_foreign_toplevel_handle_v1_set_activated(
            window->wlr_handle, activated
        );
    }
}

void window_set_maximized(struct Window *window, bool maximized) {
    if (window == NULL || window->closed) return;
    if (window->fullscreen) return;
    if (window->maximized == maximized) return;

    if (maximized) {
        if (window->pos_valid) {
            window->max_saved_x = window->x;
            window->max_saved_y = window->y;
            window->max_saved_w = window->width;
            window->max_saved_h = window->height;
        } else if (window->has_target) {
            window->max_saved_x = window->target_box.x;
            window->max_saved_y = window->target_box.y;
            window->max_saved_w = window->target_box.width;
            window->max_saved_h = window->target_box.height;
        } else {
            window->max_saved_x = 0;
            window->max_saved_y = 0;
            window->max_saved_w = window->width > 0 ? window->width : 100;
            window->max_saved_h = window->height > 0 ? window->height : 100;
        }
    }

    window->maximized = maximized;

    if (window->xdg_toplevel && window->xdg_toplevel->base->initialized) {
        wlr_xdg_toplevel_set_maximized(window->xdg_toplevel, maximized);
    }
#ifdef XWAYLAND
    if (window->is_xwayland && window->xwayland_surface) {
        wlr_xwayland_surface_set_maximized(
            window->xwayland_surface, maximized, maximized
        );
    }
#endif

    if (!maximized) {
        window_apply_target(
            window, window->max_saved_x, window->max_saved_y,
            window->max_saved_w, window->max_saved_h
        );
    }

    wm_mark_layout_dirty();
}

void window_toggle_maximized(struct Window *window) {
    if (window == NULL || window->closed) return;
    window_set_maximized(window, !window->maximized);
}

void window_set_fullscreen(struct Window *window, bool fullscreen) {
    if (window->closed || window->fullscreen == fullscreen) return;

#ifdef XWAYLAND
    if (!window->is_xwayland && !window->xdg_toplevel->base->initialized)
        return;
#else
    if (!window->xdg_toplevel->base->initialized) return;
#endif

    if (fullscreen) {
        window->fs_x = window->x;
        window->fs_y = window->y;
        window->fs_w = window->width;
        window->fs_h = window->height;
    }

    window->fullscreen = fullscreen;

#ifdef XWAYLAND
    if (window->is_xwayland) {
        wlr_xwayland_surface_set_fullscreen(
            window->xwayland_surface, fullscreen
        );
    } else
#endif
    {
        wlr_xdg_toplevel_set_fullscreen(window->xdg_toplevel, fullscreen);
    }

    window_update_corners(window);
    deco_update_for_layout(window);

    window->has_target = false;
    window_reparent(window, !layouts_is_tiled(wm.layout));

    if (!fullscreen && !layouts_is_tiled(wm.layout)) { // floating
        window_apply_target(                           // restore from saved
            window, window->fs_x, window->fs_y, window->fs_w, window->fs_h
        );
    }

    if (fullscreen) {
        struct Output *o = window->output;
        if (o == NULL) o = output_primary();
        if (o != NULL) {
            window_apply_target(
                window, o->pos_x, o->pos_y, o->width, o->height
            );
        }

        window_raise(window);
    }

    idle_update_inhibit();
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
        window->wlr_handle, window_title(window)
    );
    wlr_foreign_toplevel_handle_v1_set_app_id(
        window->wlr_handle, window_app_id(window)
    );

    struct Output *o = window->output;
    if (o != NULL && o->wlr_output != NULL) {
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
        .title = window_title(window),
        .app_id = window_app_id(window),
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
        .title = window_title(window),
        .app_id = window_app_id(window),
    };

    wlr_ext_foreign_toplevel_handle_v1_update_state(
        window->foreign_handle, &state
    );
}

static void window_apply_rules(struct Window *window) {
    if (window == NULL || window->closed || window->rules_applied) return;
    window->rules_applied = true;

    struct WindowRuleActions act;
    if (!rules_match(window_app_id(window), window_title(window), &act)) return;

    if (act.has_space) wm_assign_new_window_space(window, act.space);
    if (act.has_blur) window->blur_enabled = act.blur;
}

static void window_do_map(struct Window *window) {
    if (window->mapped) return;
    window->mapped = true;

    window_apply_rules(window);

    if (window->output != NULL && !window->space_hidden
        && window->space != window->output->current_space) {
        wm_switch_space_on(window->output, window->space);
    }

    wlr_handle_create(window);
    foreign_handle_create(window);
    wm_mark_layout_dirty();

    wlr_scene_node_set_enabled(&window->scene_tree->node, true);
    wlr_scene_node_set_enabled(&window->popup_tree->node, true);

    window_update_corners(window);
    window_set_tiled(window, layouts_is_tiled(wm.layout));
    deco_update_for_layout(window);

    if (window->space_hidden) {
        wl_list_remove(&window->focus_link);
        wl_list_insert(&wm.focus_stack, &window->focus_link);
    } else {
        seat_focus(window);
    }

    ipc_notify_window_opened(window);
    idle_update_inhibit();
}

static void window_do_unmap(struct Window *window) {
    if (!window->mapped) return;

    window->mapped = false;

    if (window->minimized) {
        window->minimized = false;
        wl_list_remove(&window->minimized_link);
        wl_list_init(&window->minimized_link);

        struct Output *out = window->output;
        if (out == NULL) out = output_focused();
        window->space_hidden =
            (out != NULL && window->space != out->current_space);
    }

    struct wlr_surface *surface = window_wlr_surface(window);
    bool had_focus = surface != NULL && server.seat != NULL
                  && server.seat->keyboard_state.focused_surface == surface;

    wlr_handle_destroy(window);
    foreign_handle_destroy(window);

    wlr_scene_node_set_enabled(&window->scene_tree->node, false);
    wlr_scene_node_set_enabled(&window->popup_tree->node, false);

    deco_set_visible(window, false);

    if (window->focus_link.next != NULL
        && window->focus_link.next != &window->focus_link) {
        wl_list_remove(&window->focus_link);
    }

    wl_list_init(&window->focus_link);
    ipc_notify_window_closed(window);

    if (had_focus) seat_focus(NULL);

    wm_mark_layout_dirty();
    idle_update_inhibit();
}

static void window_handle_map(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, map);
    window_do_map(window);
}

static void window_handle_unmap(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, unmap);
    window_do_unmap(window);
}

static void window_apply_pending_icon(struct Window *window) {
    if (window->pending_icon == NULL && !window->pending_icon_cleared) return;

    bool changed = window->icon != NULL || window->pending_icon != NULL;

    icon_free(window->icon);
    free(window->icon);

    window->icon = window->pending_icon;
    window->pending_icon = NULL;
    window->pending_icon_cleared = false;

    if (changed) ipc_notify_window_meta(window);
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
        bool resized = window->width != w || window->height != h;

        window->width = w;
        window->height = h;

        if (!window->anim.running) window_clear_clip(window);
        if (resized) deco_update_position(window);
    }

    window_apply_pending_icon(window);

    if (!window->mapped && surf->surface->mapped) window_do_map(window);

    if (surf->surface->mapped) window_update_corners(window);

    if (!wl_list_empty(&surf->surface->current.frame_callback_list)) {
        struct Output *out = window->output;
        if (out == NULL) out = output_primary();
        if (out != NULL && out->wlr_output != NULL) {
            wlr_output_schedule_frame(out->wlr_output);
        }
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
    (void)l;
    (void)data;
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

    deco_update_title(window);
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

struct Window *window_from_id(uint64_t id) {
    if (id == 0) return NULL;
    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->id == id && !w->closed) return w;
    }

    return NULL;
}

struct Window *window_from_scene_node(const struct wlr_scene_node *node) {
    struct NodeTag *tag = node_tag_find(node);
    if (tag == NULL || tag->type != NODE_WINDOW) return NULL;

    struct Window *w = wl_container_of(tag, w, node_tag);
    return w;
}

void window_icon_set_pending(struct Window *w, struct Icon *icon) {
    if (w == NULL || w->closed) {
        icon_free(icon);
        free(icon);
        return;
    }

    icon_free(w->pending_icon);
    free(w->pending_icon);

    w->pending_icon = icon;
    w->pending_icon_cleared = (icon == NULL);
}

void window_create(struct wlr_xdg_toplevel *xdg_toplevel) {
    struct Window *window = calloc(1, sizeof(*window));
    if (window == NULL) return;

    window->last_tl = -1;
    window->last_tr = -1;
    window->last_bl = -1;
    window->last_br = -1;
    window->size_per = 1.0f;
    window->maximized = false;
    window->id = ++wm.next_window_id;
    window->xdg_toplevel = xdg_toplevel;
    window->output = output_focused();
    window->space = window->output ? window->output->current_space : 1;
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
        xdg_toplevel->base->data = NULL;
        wlr_scene_node_destroy(&window->scene_tree->node);
        free(window);
        return;
    }

    window->popup_tree->node.data = &window->node_tag;

    window_update_corners(window);
    wlr_scene_node_set_enabled(&window->scene_tree->node, false);
    wlr_scene_node_set_enabled(&window->popup_tree->node, false);

    window->title = strdup(xdg_toplevel->title ? xdg_toplevel->title : "");
    window->app_id = strdup(xdg_toplevel->app_id ? xdg_toplevel->app_id : "");

    if (window->title == NULL || window->app_id == NULL) {
        xdg_toplevel->base->data = NULL;
        wlr_scene_node_destroy(&window->scene_tree->node);
        wlr_scene_node_destroy(&window->popup_tree->node);
        free(window->title);
        free(window->app_id);
        free(window);
        return;
    }

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
    wl_list_init(&window->minimized_link);
    wl_list_insert(wm.windows.prev, &window->link);
}

void window_destroy(struct Window *window) {
    if (window->closed) return;
    if (window->mapped) window_do_unmap(window);

    window->closed = true;

    if (window->xwayland_surface != NULL) window->xwayland_surface->data = NULL;

    icon_free(window->icon);
    free(window->icon);

    icon_free(window->pending_icon);
    free(window->pending_icon);

    cursor_window_destroyed(window);
    seat_resize_mode_window_destroyed(window);

    if (window->focus_link.next != NULL
        && window->focus_link.next != &window->focus_link) {
        wl_list_remove(&window->focus_link);
    }

    wl_list_init(&window->focus_link);

#ifdef XWAYLAND
    if (window->is_xwayland) {
        xw_unbind(window);

        wl_list_remove(&window->xw_associate.link);
        wl_list_remove(&window->xw_dissociate.link);
        wl_list_remove(&window->xw_destroy.link);
        wl_list_remove(&window->xw_request_configure.link);
        wl_list_remove(&window->xw_request_move.link);
        wl_list_remove(&window->xw_request_resize.link);
        wl_list_remove(&window->xw_request_maximize.link);
        wl_list_remove(&window->xw_request_fullscreen.link);
        wl_list_remove(&window->xw_request_minimize.link);
        wl_list_remove(&window->xw_request_activate.link);
        wl_list_remove(&window->xw_request_close.link);
        wl_list_remove(&window->xw_set_title.link);
        wl_list_remove(&window->xw_set_class.link);
    } else
#endif
    {
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
    }

    if (window->scene_tree != NULL) {
        wlr_scene_node_destroy(&window->scene_tree->node);
        window->scene_tree = NULL;
    }

    if (window->popup_tree != NULL) {
        wlr_scene_node_destroy(&window->popup_tree->node);
        window->popup_tree = NULL;
    }

    wlr_handle_destroy(window);

    if (window->link.next == NULL || window->link.prev == NULL) {
        log_warn("window_destroy: window->link already unlinked");
    } else {
        wl_list_remove(&window->link);
        wl_list_init(&window->link);
    }

    deco_destroy(window);
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
        if (w->closed || !w->mapped || w->space_hidden) continue;
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

    deco_update_position(window);

#ifdef XWAYLAND
    if (window->is_xwayland && window->xwayland_surface != NULL
        && window->width > 0 && window->height > 0) {
        wlr_xwayland_surface_configure(
            window->xwayland_surface, (int16_t)x, (int16_t)y,
            (uint16_t)window->width, (uint16_t)window->height
        );
    }
#endif
}

void window_set_size(struct Window *window, int32_t w, int32_t h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    if (window->prop_valid && window->prop_w == w && window->prop_h == h)
        return;

    window->prop_w = w;
    window->prop_h = h;
    window->prop_valid = true;

#ifdef XWAYLAND
    if (window->is_xwayland) {
        int32_t tx = window->pos_valid
                       ? window->x
                       : (window->has_target ? window->target_box.x : 0);

        int32_t ty = window->pos_valid
                       ? window->y
                       : (window->has_target ? window->target_box.y : 0);

        wlr_xwayland_surface_configure(
            window->xwayland_surface, (int16_t)tx, (int16_t)ty, (uint16_t)w,
            (uint16_t)h
        );

        window->width = w;
        window->height = h;
        deco_update_position(window);

        return;
    }
#endif

    struct wlr_xdg_surface *surf = window->xdg_toplevel->base;
    if (!surf->initialized) { return; }
    wlr_xdg_toplevel_set_size(window->xdg_toplevel, w, h);
    deco_update_position(window);
}

void window_set_tiled(struct Window *window, bool tiled) {
#ifdef XWAYLAND
    if (window->is_xwayland) {
        (void)tiled;
        return;
    }
#endif
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
#ifdef XWAYLAND
    if (window->is_xwayland) {
        if (window->xw_surface_tree == NULL) return;

        struct wlr_box clip = {.x = 0, .y = 0, .width = w, .height = h};
        wlr_scene_subsurface_tree_set_clip(
            &window->xw_surface_tree->node, &clip
        );

        return;
    }
#endif
    struct wlr_box geo = window->xdg_toplevel->base->geometry;
    struct wlr_box clip = {.x = geo.x, .y = geo.y, .width = w, .height = h};
    wlr_scene_subsurface_tree_set_clip(&window->scene_tree->node, &clip);
}

void window_clear_clip(struct Window *window) {
#ifdef XWAYLAND
    if (window->is_xwayland) {
        if (window->xw_surface_tree == NULL) return;
        wlr_scene_subsurface_tree_set_clip(
            &window->xw_surface_tree->node, NULL
        );

        return;
    }
#endif
    struct wlr_box geo = window->xdg_toplevel->base->geometry;
    if (geo.width <= 0 || geo.height <= 0) {
        wlr_scene_subsurface_tree_set_clip(&window->scene_tree->node, NULL);
        return;
    }

    wlr_scene_subsurface_tree_set_clip(&window->scene_tree->node, &geo);
}

void window_close(struct Window *window) {
#ifdef XWAYLAND
    if (window->is_xwayland) {
        wlr_xwayland_surface_close(window->xwayland_surface);
        return;
    }
#endif
    wlr_xdg_toplevel_send_close(window->xdg_toplevel);
}

void window_raise(struct Window *window) {
    wlr_scene_node_raise_to_top(&window->scene_tree->node);
    deco_raise(window);

    if (window->popup_tree != NULL) {
        wlr_scene_node_raise_to_top(&window->popup_tree->node);
    }
}

void window_reparent(struct Window *window, bool to_float) {
    struct wlr_scene_tree *target =
        window->fullscreen ? server.fullscreen_tree
                           : (to_float ? server.float_tree : server.tile_tree);

    if (window->scene_tree->node.parent == target) return;
    wlr_scene_node_reparent(&window->scene_tree->node, target);

    // Restack the tab right above its window in the new parent.
    deco_raise(window);
}

#ifdef XWAYLAND
static void xw_handle_map(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, xw_map);
    window_do_map(window);
}

static void xw_handle_unmap(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, xw_unmap);
    window_do_unmap(window);
}

static void xw_handle_tree_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, xw_tree_destroy);
    wl_list_remove(&window->xw_tree_destroy.link);
    window->xw_surface_tree = NULL;
}

static void xw_unbind(struct Window *window) {
    if (window->xw_bound) {
        wl_list_remove(&window->xw_map.link);
        wl_list_remove(&window->xw_unmap.link);
        window->xw_bound = false;
    }

    if (window->xw_surface_tree != NULL) {
        wl_list_remove(&window->xw_tree_destroy.link);
        wlr_scene_node_destroy(&window->xw_surface_tree->node);
        window->xw_surface_tree = NULL;
    }
}

static void xw_bind(struct Window *window) {
    struct wlr_surface *surface = window->xwayland_surface->surface;
    if (window->closed || window->xw_bound || surface == NULL) return;

    window->xw_surface_tree =
        wlr_scene_subsurface_tree_create(window->scene_tree, surface);
    if (window->xw_surface_tree == NULL) {
        log_err("unable to create scene tree for x11 window: OOM");
        wlr_xwayland_surface_close(window->xwayland_surface);
        return;
    }

    window->xw_tree_destroy.notify = xw_handle_tree_destroy;
    wl_signal_add(
        &window->xw_surface_tree->node.events.destroy, &window->xw_tree_destroy
    );

    window->xw_map.notify = xw_handle_map;
    wl_signal_add(&surface->events.map, &window->xw_map);
    window->xw_unmap.notify = xw_handle_unmap;
    wl_signal_add(&surface->events.unmap, &window->xw_unmap);

    window->xw_bound = true;
    window->last_tl = window->last_tr = -1;
    window->last_bl = window->last_br = -1;

    window_update_corners(window);

    if (surface->mapped) { window_do_map(window); }
}

static void xw_handle_associate(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, xw_associate);
    xw_bind(window);
}

static void xw_handle_dissociate(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, xw_dissociate);
    if (window->mapped) window_do_unmap(window);
    xw_unbind(window);
}

static void xw_handle_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, xw_destroy);
    window_destroy(window);
}

static void xw_handle_request_configure(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, xw_request_configure);
    if (window->closed || window->xwayland_surface == NULL) return;

    int32_t x, y, w, h;
    if (window->has_target) {
        x = window->target_box.x;
        y = window->target_box.y;
        w = window->target_box.width;
        h = window->target_box.height;
    } else {
        x = window->pos_valid ? window->x : 0;
        y = window->pos_valid ? window->y : 0;
        w = window->xwayland_surface->width;
        h = window->xwayland_surface->height;
    }

    if (w < 1) w = 1;
    if (h < 1) h = 1;

    wlr_xwayland_surface_configure(
        window->xwayland_surface, (int16_t)x, (int16_t)y, (uint16_t)w,
        (uint16_t)h
    );
}

static void xw_handle_request_move(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, xw_request_move);
    if (!layouts_is_tiled(wm.layout)) {
        cursor_begin_interactive(window, SEAT_OP_MOVE, 0);
    }
}

static void xw_handle_request_resize(struct wl_listener *l, void *data) {
    struct wlr_xwayland_resize_event *ev = data;

    struct Window *window = wl_container_of(l, window, xw_request_resize);
    if (!layouts_is_tiled(wm.layout)) {
        cursor_begin_interactive(window, SEAT_OP_RESIZE, ev->edges);
    }
}

static void xw_handle_request_maximize(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, xw_request_maximize);

    if (window->xwayland_surface) {
        wlr_xwayland_surface_set_maximized(
            window->xwayland_surface, false, false
        );
    }
}

static void xw_handle_request_fullscreen(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, xw_request_fullscreen);
    if (!window->xwayland_surface) return;
    window_set_fullscreen(window, window->xwayland_surface->fullscreen);
}

static void xw_handle_request_minimize(struct wl_listener *l, void *data) {
    (void)l;
    (void)data;
}

static void xw_handle_request_activate(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, xw_request_activate);
    if (window->closed) return;
    if (window->space_hidden) wm_switch_space(window->space);
    seat_focus(window);
}

static void xw_handle_request_close(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, xw_request_close);
    window_close(window);
}

static void xw_handle_set_title(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, xw_set_title);
    const char *new_title =
        window->xwayland_surface->title ? window->xwayland_surface->title : "";

    const char *old_title = window->title ? window->title : "";
    if (strcmp(new_title, old_title) == 0) return;

    char *copy = strdup(new_title);
    if (copy == NULL) {
        log_err("unable to cache x11 window title: OOM");
        return;
    }

    free(window->title);
    window->title = copy;

    if (window->wlr_handle) {
        wlr_foreign_toplevel_handle_v1_set_title(window->wlr_handle, new_title);
    }

    deco_update_title(window);
    foreign_handle_update(window);
    ipc_notify_window_meta(window);
}

static void xw_handle_set_class(struct wl_listener *l, void *data) {
    (void)data;

    struct Window *window = wl_container_of(l, window, xw_set_class);
    const char *new_id =
        window->xwayland_surface->class ? window->xwayland_surface->class : "";

    const char *old_id = window->app_id ? window->app_id : "";
    if (strcmp(new_id, old_id) == 0) return;

    char *copy = strdup(new_id);
    if (copy == NULL) {
        log_err("Unable to cache x11 window class: OOM");
        return;
    }

    free(window->app_id);
    window->app_id = copy;

    if (window->wlr_handle) {
        wlr_foreign_toplevel_handle_v1_set_app_id(window->wlr_handle, new_id);
    }

    foreign_handle_update(window);
    ipc_notify_window_meta(window);
}

void window_create_xwayland(struct wlr_xwayland_surface *xw) {
    struct Window *window = calloc(1, sizeof(*window));
    if (window == NULL) {
        wlr_xwayland_surface_close(xw);
        return;
    }

    window->last_tl = -1;
    window->last_tr = -1;
    window->last_bl = -1;
    window->last_br = -1;
    window->size_per = 1.0f;
    window->maximized = false;
    window->id = ++wm.next_window_id;
    window->is_xwayland = true;
    window->xwayland_surface = xw;
    window->output = output_focused();
    window->space = window->output ? window->output->current_space : 1;
    window->node_tag.type = NODE_WINDOW;

    struct wlr_scene_tree *parent = layer_for_layout(wm.layout);
    window->scene_tree = wlr_scene_tree_create(parent);
    if (window->scene_tree == NULL) {
        free(window);
        wlr_xwayland_surface_close(xw);
        return;
    }

    xw->data = window->scene_tree;
    window->scene_tree->node.data = &window->node_tag;

    window->popup_tree = wlr_scene_tree_create(server.popup_tree);
    if (window->popup_tree == NULL) {
        xw->data = NULL;
        wlr_scene_node_destroy(&window->scene_tree->node);
        free(window);
        wlr_xwayland_surface_close(xw);
        return;
    }

    window->popup_tree->node.data = &window->node_tag;

    wlr_scene_node_set_enabled(&window->scene_tree->node, false);
    wlr_scene_node_set_enabled(&window->popup_tree->node, false);

    window->title = strdup(xw->title ? xw->title : "");
    window->app_id = strdup(xw->class ? xw->class : "");
    if (window->title == NULL || window->app_id == NULL) {
        xw->data = NULL;
        wlr_scene_node_destroy(&window->scene_tree->node);
        wlr_scene_node_destroy(&window->popup_tree->node);
        free(window->title);
        free(window->app_id);
        free(window);
        wlr_xwayland_surface_close(xw);
        return;
    }

    wl_list_init(&window->focus_link);
    wl_list_init(&window->minimized_link);

    window->xw_associate.notify = xw_handle_associate;
    wl_signal_add(&xw->events.associate, &window->xw_associate);

    window->xw_dissociate.notify = xw_handle_dissociate;
    wl_signal_add(&xw->events.dissociate, &window->xw_dissociate);

    window->xw_destroy.notify = xw_handle_destroy;
    wl_signal_add(&xw->events.destroy, &window->xw_destroy);

    window->xw_request_configure.notify = xw_handle_request_configure;
    wl_signal_add(&xw->events.request_configure, &window->xw_request_configure);

    window->xw_request_move.notify = xw_handle_request_move;
    wl_signal_add(&xw->events.request_move, &window->xw_request_move);

    window->xw_request_resize.notify = xw_handle_request_resize;
    wl_signal_add(&xw->events.request_resize, &window->xw_request_resize);

    window->xw_request_maximize.notify = xw_handle_request_maximize;
    wl_signal_add(&xw->events.request_maximize, &window->xw_request_maximize);

    window->xw_request_fullscreen.notify = xw_handle_request_fullscreen;
    wl_signal_add(
        &xw->events.request_fullscreen, &window->xw_request_fullscreen
    );

    window->xw_request_minimize.notify = xw_handle_request_minimize;
    wl_signal_add(&xw->events.request_minimize, &window->xw_request_minimize);

    window->xw_request_activate.notify = xw_handle_request_activate;
    wl_signal_add(&xw->events.request_activate, &window->xw_request_activate);

    window->xw_request_close.notify = xw_handle_request_close;
    wl_signal_add(&xw->events.request_close, &window->xw_request_close);

    window->xw_set_title.notify = xw_handle_set_title;
    wl_signal_add(&xw->events.set_title, &window->xw_set_title);

    window->xw_set_class.notify = xw_handle_set_class;
    wl_signal_add(&xw->events.set_class, &window->xw_set_class);

    wl_list_insert(wm.windows.prev, &window->link);
    xw_bind(window);
}

#endif
