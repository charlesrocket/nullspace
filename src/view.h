#ifndef VIEW_H
#define VIEW_H

#include "animation.h"
#include "include/layouts.h"

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

void view_create(struct wlr_xdg_toplevel *xdg_toplevel);
void view_destroy(struct Window *view);

struct Window *view_focused(void);
struct Window *view_at(
    double lx, double ly, struct wlr_surface **surface, double *sx, double *sy
);

void view_set_fullscreen(struct Window *view, bool fullscreen);
void view_set_position(struct Window *view, int32_t x, int32_t y);
void view_set_size(struct Window *view, int32_t w, int32_t h);
void view_set_tiled(struct Window *view, bool tiled);
void view_set_clip(struct Window *view, int32_t w, int32_t h);
void view_set_activated(struct Window *view, bool activated);
void view_clear_clip(struct Window *view);

void view_close(struct Window *view);
void view_raise(struct Window *view);
void view_reparent(struct Window *view, bool to_float);

#endif // VIEW_H
