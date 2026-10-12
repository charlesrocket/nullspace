#ifndef SERVER_H
#define SERVER_H

#include <scenefx/types/wlr_scene.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

struct Output;
struct Window;

struct NsServer {
    struct wl_display *display;
    struct wl_event_loop *event_loop;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_session *session;

    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_scene_tree *background_tree;
    struct wlr_scene_tree *wallpaper_blur_tree;
    struct wlr_scene_tree *bottom_tree;
    struct wlr_scene_tree *tile_tree;
    struct wlr_scene_tree *float_tree;
    struct wlr_scene_tree *top_tree;
    struct wlr_scene_tree *overlay_tree;

    struct wlr_output_layout *output_layout;

    struct wlr_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wl_listener request_set_cursor_shape;

    struct wlr_layer_shell_v1 *layer_shell;
    struct wl_listener new_layer_surface;

    struct wlr_xdg_shell *xdg_shell;
    struct wlr_xdg_decoration_manager_v1 *xdg_decoration;

    struct wl_list outputs;
    struct wl_list keyboards;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;

    struct wlr_seat *seat;

    struct wl_listener new_output;
    struct wl_listener new_input;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;
    struct wl_listener new_xdg_decoration;

    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;

    struct wl_listener request_set_selection;
    struct wl_listener request_start_drag;
    struct wl_listener request_set_cursor;
};

extern struct NsServer server;

int server_init(struct wl_display *display);
void server_detach_listeners(void);
void server_finish(void);

struct Window *window_from_scene_node(struct wlr_scene_node *node);
struct Output *output_from_wlr_output(struct wlr_output *wlr_output);

#endif // SERVER_H
