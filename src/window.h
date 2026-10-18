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

struct Window {
    struct wlr_xdg_toplevel *xdg_toplevel;
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

    enum Layout decoration_state;
    bool decoration_state_set;

    int32_t saved_x, saved_y;
    int32_t x, y;
    int32_t width, height;
    int32_t prop_w, prop_h;
    int32_t reveal_dir, hide_dir;
    int32_t fs_x, fs_y, fs_w, fs_h; // geometry to restore (floating layout)

    int space;

    char *title;
    char *app_id;

    bool initial_configure_sent;
    bool mapped;
    bool prop_valid;
    bool pos_valid;
    bool has_target;
    bool closed;
    bool fullscreen;
    bool space_hidden;
};

void window_create(struct wlr_xdg_toplevel *xdg_toplevel);
void window_destroy(struct Window *window);

struct Window *window_focused(void);
struct Window *window_at(
    double lx, double ly, struct wlr_surface **surface, double *sx, double *sy
);

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
