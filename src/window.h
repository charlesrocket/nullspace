#pragma once

#include "animation.h"
#include "server.h"

#include <nullspace/layouts.h>
#include <scenefx/types/wlr_scene.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_xdg_shell.h>

struct wlr_xwayland_surface;

struct Window {
    struct wlr_xdg_toplevel *xdg_toplevel;

#ifdef XWAYLAND
    struct wlr_xwayland_surface *xwayland_surface;
#endif

    struct wlr_scene_tree *scene_tree;
    struct wlr_scene_tree *popup_tree;

    struct wl_list link;       // WindowManager.windows
    struct wl_list focus_link; // WindowManager.focus_stack
    struct wlr_ext_foreign_toplevel_handle_v1 *foreign_handle;
    struct wlr_foreign_toplevel_handle_v1 *wlr_handle;

    struct NodeTag node_tag;

    struct Animation anim;
    struct AnimationBox target_box;

    struct wl_listener wlr_request_activate;
    struct wl_listener wlr_request_close;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
    struct wl_listener request_minimize;
    struct wl_listener set_title;
    struct wl_listener set_app_id;

#ifdef XWAYLAND
    struct wl_listener xw_map;
    struct wl_listener xw_unmap;
    struct wl_listener xw_destroy;
    struct wl_listener xw_request_configure;
    struct wl_listener xw_request_move;
    struct wl_listener xw_request_resize;
    struct wl_listener xw_request_maximize;
    struct wl_listener xw_request_fullscreen;
    struct wl_listener xw_request_minimize;
    struct wl_listener xw_request_activate;
    struct wl_listener xw_request_close;
    struct wl_listener xw_set_title;
    struct wl_listener xw_set_class;
    struct wl_listener xw_associate;
    struct wl_listener xw_dissociate;
    struct wl_listener xw_tree_destroy;
    struct wlr_scene_tree *xw_surface_tree;

    bool xw_bound; // `xw_map`/`xw_unmap` registered
#endif

    enum Layout decoration_state;

    int32_t saved_x, saved_y;
    int32_t x, y;
    int32_t width, height;
    int32_t prop_w, prop_h;
    int32_t reveal_dir, hide_dir;
    int32_t fs_x, fs_y, fs_w, fs_h; // geometry to restore (floating layout)

    int space;

    char *title, *app_id;

    bool initial_configure_sent, rules_applied;
    bool decoration_state_set;
    bool prop_valid, pos_valid;
    bool has_target, closed, mapped;
    bool fullscreen, space_hidden;
#ifdef XWAYLAND
    bool is_xwayland;
#endif
};

void window_create(struct wlr_xdg_toplevel *xdg_toplevel);

#ifdef XWAYLAND
void window_create_xwayland(struct wlr_xwayland_surface *xw);
#endif

void window_destroy(struct Window *window);

struct Window *window_from_scene_node(struct wlr_scene_node *node);
struct Window *window_focused(void);
struct Window *window_at(
    double lx, double ly, struct wlr_surface **surface, double *sx, double *sy
);

struct wlr_surface *window_wlr_surface(const struct Window *window);
const char *window_title(const struct Window *window);
const char *window_app_id(const struct Window *window);

void window_apply_target(
    struct Window *w, int32_t nx, int32_t ny, int32_t nw, int32_t nh
);

void window_set_fullscreen(struct Window *window, bool fullscreen);
void window_set_position(struct Window *window, int32_t x, int32_t y);
void window_set_size(struct Window *window, int32_t w, int32_t h);
void window_set_tiled(struct Window *window, bool tiled);
void window_set_clip(struct Window *window, int32_t w, int32_t h);
void window_set_activated(struct Window *window, bool activated);
void window_clear_clip(struct Window *window);

void window_close(struct Window *window);
void window_raise(struct Window *window);
void window_reparent(struct Window *window, bool to_float);
