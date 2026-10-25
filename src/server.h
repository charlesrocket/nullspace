#pragma once

#include <scenefx/types/wlr_scene.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

struct wlr_output_power_manager_v1;
struct wlr_xdg_activation_v1;
struct wlr_ext_foreign_toplevel_list_v1;
struct wlr_foreign_toplevel_manager_v1;
struct wlr_idle_inhibit_manager_v1;
struct wlr_idle_notifier_v1;
struct wlr_xwayland;

struct Window;
struct Output;

struct NsServer {
    struct wl_display *display;
    struct wl_event_loop *event_loop;

    struct wlr_backend *backend;
    struct wlr_session *session;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;

    struct wlr_compositor *compositor;

    struct wlr_output_layout *output_layout;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;

    struct wlr_scene_tree *background_tree;
    struct wlr_scene_tree *bottom_tree;
    struct wlr_scene_tree *tile_tree;
    struct wlr_scene_tree *float_tree;
    struct wlr_scene_tree *popup_tree;
    struct wlr_scene_tree *top_tree;
    struct wlr_scene_tree *overlay_tree;
    struct wlr_scene_tree *fullscreen_tree;
    struct wlr_scene_tree *content_tree;
    struct wlr_scene_tree *lock_tree;
    struct wlr_scene_rect *lock_bg;

    struct wlr_layer_shell_v1 *layer_shell;

    struct wlr_xdg_activation_v1 *xdg_activation;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_xdg_decoration_manager_v1 *xdg_decoration;

    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;
    struct wl_listener new_layer_surface;
    struct wl_listener new_xdg_decoration;

    struct wlr_seat *seat;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;

    struct wl_listener request_set_selection;
    struct wl_listener request_set_primary_selection;
    struct wl_listener request_start_drag;
    struct wl_listener request_set_cursor;

    struct wlr_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wl_listener request_set_cursor_shape;

    struct wl_listener new_output;
    struct wl_listener new_input;
    struct wl_list outputs;
    struct wl_list keyboards;

    struct wlr_ext_foreign_toplevel_list_v1 *foreign_toplevel_list;
    struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;

    struct wlr_output_power_manager_v1 *output_power_manager;
    struct wl_listener output_power_set_mode;

    struct wlr_idle_notifier_v1 *idle_notifier;
    struct wlr_idle_inhibit_manager_v1 *idle_inhibit_manager;
    struct wl_listener new_idle_inhibitor;

#ifdef XWAYLAND
    struct wlr_xwayland *xwayland;
#endif

    bool locked;
};

extern struct NsServer server;

enum NodeType { NODE_WINDOW = 1, NODE_LAYER_SURFACE };

struct NodeTag {
    enum NodeType type;
};

struct NodeTag *node_tag_find(struct wlr_scene_node *node);

int server_init(struct wl_display *display);
void server_detach_listeners(void);
void server_finish(void);
void server_notify_activity(void);

void idle_update_inhibit(void);
