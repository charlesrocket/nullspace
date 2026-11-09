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
#include <wlr/types/wlr_pointer_gestures_v1.h>
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

struct Server {
    struct wl_display* display;
    struct wl_event_loop* event_loop;

    struct wlr_backend* backend;
    struct wlr_session* session;
    struct wlr_renderer* renderer;
    struct wlr_allocator* allocator;
    struct wlr_compositor* compositor;

    struct wlr_output_layout* output_layout;
    struct wlr_scene* scene;
    struct wlr_scene_output_layout* scene_layout;

    struct wlr_scene_tree* background_tree; // wallpaper
    struct wlr_scene_tree* bottom_tree;     // lower
    struct wlr_scene_tree* tile_tree;       // normal
    struct wlr_scene_tree* float_tree;      // floating
    struct wlr_scene_tree* popup_tree;      // menus
    struct wlr_scene_tree* top_tree;        // panels
    struct wlr_scene_tree* overlay_tree;    // immediate
    struct wlr_scene_tree* fullscreen_tree; // expanded
    struct wlr_scene_tree* overview_tree;   // control
    struct wlr_scene_tree* content_tree;    // session
    struct wlr_scene_tree* lock_tree;       // locked

    struct wlr_scene_rect* lock_bg;

    struct wlr_xdg_activation_v1* xdg_activation;
    struct wlr_xdg_shell* xdg_shell;
    struct wlr_xdg_decoration_manager_v1* xdg_decoration;

    struct wlr_seat* seat;

    struct wlr_content_type_manager_v1* content_type_manager;

    struct wlr_output_power_manager_v1* output_power_manager;
    struct wlr_idle_inhibit_manager_v1* idle_inhibit_manager;
    struct wlr_idle_notifier_v1* idle_notifier;
    struct wlr_layer_shell_v1* layer_shell;
    struct wlr_ext_foreign_toplevel_list_v1* foreign_toplevel_list;
    struct wlr_foreign_toplevel_manager_v1* foreign_toplevel_manager;
    struct wlr_relative_pointer_manager_v1* relative_pointer_manager;
    struct wlr_pointer_constraints_v1* pointer_constraints;
    struct wlr_pointer_gestures_v1* pointer_gestures;
    struct wlr_cursor_shape_manager_v1* cursor_shape_manager;
    struct wlr_xcursor_manager* cursor_mgr;
    struct wlr_cursor* cursor;

#ifdef XWAYLAND
    struct wlr_xwayland* xwayland;
#endif

    struct wl_listener cursor_motion, cursor_motion_absolute, cursor_button,
        cursor_axis, output_power_set_mode, cursor_frame, new_output, new_input,
        new_idle_inhibitor, new_xdg_toplevel, new_xdg_popup, new_layer_surface,
        new_xdg_decoration, request_set_selection,
        request_set_primary_selection, request_set_cursor_shape,
        request_start_drag, request_set_cursor, start_drag, cursor_swipe_begin,
        cursor_swipe_update, cursor_swipe_end, cursor_pinch_begin,
        cursor_pinch_update, cursor_pinch_end, cursor_hold_begin,
        cursor_hold_end;

    struct wl_list keyboards, outputs;

    bool locked, overview_active;
};

extern struct Server server;

enum NodeType { NODE_WINDOW = 1, NODE_LAYER_SURFACE };

struct NodeTag {
    enum NodeType type;
};

struct NodeTag* node_tag_find(const struct wlr_scene_node* node);

int server_init(struct wl_display* display);
void server_detach_listeners(void);
void server_finish(void);
void server_notify_activity(void);

void idle_update_inhibit(void);
