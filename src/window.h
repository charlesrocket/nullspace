#pragma once

#include "animation.h"
#include "decoration.h"
#include "icon.h"
#include "server.h"

#include <nullspace/layouts.h>
#include <scenefx/types/wlr_scene.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>

struct wlr_xwayland_surface;

struct Window {
    struct wlr_xdg_toplevel *xdg_toplevel;

#ifdef XWAYLAND
    struct wlr_xwayland_surface *xwayland_surface;
#endif

    struct wlr_scene_tree *scene_tree, *popup_tree;

    struct wl_list link;           // WindowManager.windows
    struct wl_list focus_link;     // WindowManager.focus_stack
    struct wl_list minimized_link; // WindowManager.minimized_windows
    struct wlr_ext_foreign_toplevel_handle_v1 *foreign_handle;
    struct wlr_foreign_toplevel_handle_v1 *wlr_handle;

    struct Output *output;
    struct NodeTag node_tag;
    struct Decoration *decoration; // ?NULL (floating/non‑fullscreen windows)
    struct Icon *icon;             // current, applied on commit
    struct Icon *pending_icon;     // ?NULL

    struct Animation anim;
    struct AnimationBox target_box;

    struct wl_listener map, unmap, commit, destroy, request_move,
        wlr_request_activate, wlr_request_close, request_resize,
        request_maximize, request_fullscreen, request_minimize, set_title,
        set_app_id;

#ifdef XWAYLAND
    struct wl_listener xw_map, xw_unmap, xw_destroy, xw_request_configure,
        xw_request_move, xw_request_resize, xw_request_maximize,
        xw_request_fullscreen, xw_request_minimize, xw_request_activate,
        xw_request_close, xw_set_title, xw_set_class, xw_associate,
        xw_dissociate, xw_tree_destroy;

    struct wlr_scene_tree *xw_surface_tree;

    bool xw_bound; // `xw_map`/`xw_unmap` registered
#endif

    enum Layout decoration_state;

    uint64_t id;

    int32_t max_saved_x, max_saved_y, max_saved_w, max_saved_h, saved_x,
        saved_y, x, y, width, height, prop_w, prop_h, reveal_dir, hide_dir,
        last_tl, last_tr, last_bl, last_br;

    int32_t fs_x, fs_y, fs_w, fs_h; // geometry to restore (floating)

    float size_per; // tiled size weight

    int space;

    char *title, *app_id;

    bool initial_configure_sent, maximized, rules_applied, pending_icon_cleared,
        decoration_state_set, prop_valid, pos_valid, has_target, closed, mapped,
        fullscreen, space_hidden, minimized, blur_enabled;

#ifdef XWAYLAND
    bool is_xwayland;
#endif
};

void window_create(struct wlr_xdg_toplevel *xdg_toplevel);
void window_destroy(struct Window *window);
void window_close(struct Window *window);
void window_raise(struct Window *window);
void window_reparent(struct Window *window, bool to_float);
void window_update_corners(struct Window *window);
void window_icon_set_pending(struct Window *w, struct Icon *icon);
void window_set_fullscreen(struct Window *window, bool fullscreen);
void window_set_position(struct Window *window, int32_t x, int32_t y);
void window_set_size(struct Window *window, int32_t w, int32_t h);
void window_set_tiled(struct Window *window, bool tiled);
void window_set_clip(struct Window *window, int32_t w, int32_t h);
void window_set_activated(struct Window *window, bool activated);
void window_set_maximized(struct Window *window, bool maximized);
void window_toggle_maximized(struct Window *window);
void window_clear_clip(struct Window *window);
void window_apply_target(
    struct Window *w, int32_t nx, int32_t ny, int32_t nw, int32_t nh
);

bool window_usable_area(const struct Window *w, struct wlr_box *out);
int32_t window_visual_top_offset(const struct Window *w);
void window_clamp_to_usable(
    struct Window *w, int32_t *x, int32_t *y, int32_t *width, int32_t *height
);

#ifdef XWAYLAND
void window_create_xwayland(struct wlr_xwayland_surface *xw);
#endif

struct Window *window_from_id(uint64_t id);
struct Window *window_from_scene_node(const struct wlr_scene_node *node);
struct Window *window_focused(void);
struct Window *window_at(
    double lx, double ly, struct wlr_surface **surface, double *sx, double *sy
);

struct wlr_surface *window_wlr_surface(const struct Window *window);
const char *window_title(const struct Window *window);
const char *window_app_id(const struct Window *window);
