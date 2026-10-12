#include "server.h"

#include "background.h"
#include "cursor.h"
#include "input.h"
#include "layers.h"
#include "layouts/layout.h"
#include "log.h"
#include "manager.h"
#include "output.h"
#include "seat.h"
#include "view.h"

#include <scenefx/render/fx_renderer/fx_renderer.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-protocols/cursor-shape-v1-enum.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>

#define LOG_TOPIC "server"

struct NsServer server = {0};

static void handle_new_output(struct wl_listener *l, void *data);
static void handle_new_input(struct wl_listener *l, void *data);
static void handle_new_xdg_toplevel(struct wl_listener *l, void *data);
static void handle_new_xdg_popup(struct wl_listener *l, void *data);
static void handle_new_xdg_decoration(struct wl_listener *l, void *data);

static void handle_request_set_selection(struct wl_listener *l, void *data);
static void handle_request_start_drag(struct wl_listener *l, void *data);
static void handle_request_set_cursor(struct wl_listener *l, void *data);

static const char *cursor_shape_name(uint32_t shape) {
    switch (shape) {
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT: return "default";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU:
            return "context-menu";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP: return "help";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER: return "pointer";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS: return "progress";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT: return "wait";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL: return "cell";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR: return "crosshair";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT: return "text";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT:
            return "vertical-text";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS: return "alias";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY: return "copy";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE: return "move";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP: return "no-drop";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED: return "not-allowed";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB: return "grab";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING: return "grabbing";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE: return "e-resize";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE: return "n-resize";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE: return "ne-resize";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE: return "nw-resize";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE: return "s-resize";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE: return "se-resize";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE: return "sw-resize";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE: return "w-resize";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE: return "ew-resize";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE: return "ns-resize";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE: return "nesw-resize";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE: return "nwse-resize";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE: return "col-resize";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE: return "row-resize";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL: return "all-scroll";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN: return "zoom-in";
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT: return "zoom-out";
        default: return "default";
    }
}

static void handle_request_set_cursor_shape(struct wl_listener *l, void *data) {
    struct NsServer *s = wl_container_of(l, s, request_set_cursor_shape);
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *ev = data;

    if (s->seat->pointer_state.focused_client != ev->seat_client) return;

    wlr_cursor_set_xcursor(
        s->cursor, s->cursor_mgr, cursor_shape_name(ev->shape)
    );
}

int server_init(struct wl_display *display) {
    server.display = display;
    server.event_loop = wl_display_get_event_loop(display);

    server.backend = wlr_backend_autocreate(server.event_loop, &server.session);
    if (server.backend == NULL) {
        log_err("Failed to create wlroots backend");
        return -1;
    }

    server.renderer = fx_renderer_create(server.backend);
    if (server.renderer == NULL) {
        log_err("Failed to create renderer");
        return -1;
    }

    if (!wlr_renderer_init_wl_display(server.renderer, display)) {
        log_err("Failed to init renderer on display");
        return -1;
    }

    server.allocator =
        wlr_allocator_autocreate(server.backend, server.renderer);
    if (server.allocator == NULL) {
        log_err("Failed to create allocator");
        return -1;
    }

    wlr_compositor_create(display, 6, server.renderer);
    wlr_subcompositor_create(display);
    wlr_data_device_manager_create(display);
    wlr_data_control_manager_v1_create(display);
    wlr_viewporter_create(display);
    wlr_xdg_activation_v1_create(display);
    wlr_relative_pointer_manager_v1_create(display);
    wlr_pointer_constraints_v1_create(display);
    wlr_server_decoration_manager_create(display);
    wlr_screencopy_manager_v1_create(display);
    wlr_ext_foreign_toplevel_list_v1_create(display, 1);

    server.output_layout = wlr_output_layout_create(display);
    wlr_xdg_output_manager_v1_create(display, server.output_layout);

    server.scene = wlr_scene_create();
    server.scene_layout =
        wlr_scene_attach_output_layout(server.scene, server.output_layout);

    server.background_tree = wlr_scene_tree_create(&server.scene->tree);
    server.wallpaper_blur_tree = wlr_scene_tree_create(&server.scene->tree);
    server.bottom_tree = wlr_scene_tree_create(&server.scene->tree);
    server.tile_tree = wlr_scene_tree_create(&server.scene->tree);
    server.float_tree = wlr_scene_tree_create(&server.scene->tree);
    server.top_tree = wlr_scene_tree_create(&server.scene->tree);
    server.overlay_tree = wlr_scene_tree_create(&server.scene->tree);

    server.xdg_shell = wlr_xdg_shell_create(display, 6);
    if (server.xdg_shell == NULL) {
        log_err("failed to create xdg-shell");
        return -1;
    }

    server.new_xdg_toplevel.notify = handle_new_xdg_toplevel;
    wl_signal_add(
        &server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel
    );

    server.new_xdg_popup.notify = handle_new_xdg_popup;
    wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

    server.xdg_decoration = wlr_xdg_decoration_manager_v1_create(display);
    server.new_xdg_decoration.notify = handle_new_xdg_decoration;
    wl_signal_add(
        &server.xdg_decoration->events.new_toplevel_decoration,
        &server.new_xdg_decoration
    );

    server.seat = wlr_seat_create(display, "default");
    if (server.seat == NULL) {
        log_err("failed to create seat");
        return -1;
    }

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

    server.cursor_motion.notify = cursor_handle_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = cursor_handle_motion_absolute;
    wl_signal_add(
        &server.cursor->events.motion_absolute, &server.cursor_motion_absolute
    );

    server.cursor_button.notify = cursor_handle_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = cursor_handle_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
    server.cursor_frame.notify = cursor_handle_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    server.request_set_selection.notify = handle_request_set_selection;
    wl_signal_add(
        &server.seat->events.request_set_selection,
        &server.request_set_selection
    );

    server.request_start_drag.notify = handle_request_start_drag;
    wl_signal_add(
        &server.seat->events.request_start_drag, &server.request_start_drag
    );

    server.request_set_cursor.notify = handle_request_set_cursor;
    wl_signal_add(
        &server.seat->events.request_set_cursor, &server.request_set_cursor
    );

    server.new_output.notify = handle_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);
    server.new_input.notify = handle_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);

    wl_list_init(&server.outputs);
    wl_list_init(&server.keyboards);

    server.layer_shell = wlr_layer_shell_v1_create(display, 4);
    if (server.layer_shell == NULL) {
        log_err("failed to create layer-shell global");
        return -1;
    }

    server.new_layer_surface.notify = layers_handle_new_surface;
    wl_signal_add(
        &server.layer_shell->events.new_surface, &server.new_layer_surface
    );

    if (!background_effect_init(display)) {
        log_warn("Background effect unavailable");
    }

    server.cursor_shape_manager =
        wlr_cursor_shape_manager_v1_create(display, 1);

    server.request_set_cursor_shape.notify = handle_request_set_cursor_shape;
    wl_signal_add(
        &server.cursor_shape_manager->events.request_set_shape,
        &server.request_set_cursor_shape
    );

    wlr_text_input_manager_v3_create(display);

    seat_init();

    if (!wlr_backend_start(server.backend)) {
        log_err("failed to start wlroots backend");
        return -1;
    }

    return 0;
}

void server_detach_listeners(void) {
    wl_list_remove(&server.new_output.link);
    wl_list_remove(&server.new_input.link);
    wl_list_remove(&server.new_layer_surface.link);
    wl_list_remove(&server.new_xdg_toplevel.link);
    wl_list_remove(&server.new_xdg_popup.link);
    wl_list_remove(&server.new_xdg_decoration.link);

    wl_list_remove(&server.cursor_motion.link);
    wl_list_remove(&server.cursor_motion_absolute.link);
    wl_list_remove(&server.cursor_button.link);
    wl_list_remove(&server.cursor_axis.link);
    wl_list_remove(&server.cursor_frame.link);

    wl_list_remove(&server.request_set_selection.link);
    wl_list_remove(&server.request_start_drag.link);
    wl_list_remove(&server.request_set_cursor.link);
    wl_list_remove(&server.request_set_cursor_shape.link);
}

void server_finish(void) {
    struct Output *out, *tmp;
    wl_list_for_each_safe(out, tmp, &server.outputs, link) output_destroy(out);

    background_effect_finish();

    if (server.cursor_mgr) wlr_xcursor_manager_destroy(server.cursor_mgr);
    if (server.cursor) wlr_cursor_destroy(server.cursor);
    if (server.output_layout) wlr_output_layout_destroy(server.output_layout);
    if (server.scene) wlr_scene_node_destroy(&server.scene->tree.node);
}

struct Window *window_from_scene_node(struct wlr_scene_node *node) {
    while (node != NULL) {
        if (node->data != NULL) return node->data;
        if (node->parent == NULL) return NULL;
        node = &node->parent->node;
    }

    return NULL;
}

struct Output *output_from_wlr_output(struct wlr_output *wlr_output) {
    struct Output *out;
    wl_list_for_each(out, &server.outputs, link) {
        if (out->wlr_output == wlr_output) return out;
    }

    return NULL;
}

static void handle_new_output(struct wl_listener *l, void *data) {
    (void)l;
    output_create((struct wlr_output *)data);
}

static void handle_new_input(struct wl_listener *l, void *data) {
    (void)l;
    seat_handle_new_input((struct wlr_input_device *)data);
}

static void handle_new_xdg_toplevel(struct wl_listener *l, void *data) {
    (void)l;
    view_create((struct wlr_xdg_toplevel *)data);
}

static void handle_new_xdg_popup(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_xdg_popup *popup = data;

    if (popup->parent == NULL) { return; }

    struct wlr_xdg_surface *parent_xdg =
        wlr_xdg_surface_try_from_wlr_surface(popup->parent);
    struct wlr_scene_tree *parent_tree = server.top_tree;

    if (parent_xdg != NULL) {
        struct Window *parent = window_from_scene_node(parent_xdg->data);
        if (parent != NULL) parent_tree = parent->scene_tree;
    } else if (popup->parent != NULL) {
        struct LayerSurface *ls = layer_surface_from_wlr_surface(popup->parent);
        if (ls != NULL) parent_tree = ls->scene_tree;
    }

    popup->base->data = wlr_scene_xdg_surface_create(parent_tree, popup->base);
}

static void handle_new_xdg_decoration(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_xdg_toplevel_decoration_v1 *deco = data;
    struct wlr_xdg_surface *surf = deco->toplevel->base;

    if (!surf->initialized) { return; }

    enum wlr_xdg_toplevel_decoration_v1_mode mode =
        layouts_is_tiled(wm.layout)
            ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
            : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;

    wlr_xdg_toplevel_decoration_v1_set_mode(deco, mode);
}

static void handle_request_set_selection(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_seat_request_set_selection_event *ev = data;
    wlr_seat_set_selection(server.seat, ev->source, ev->serial);
}

static void handle_request_start_drag(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_seat_request_start_drag_event *ev = data;
    if (wlr_seat_validate_pointer_grab_serial(
            server.seat, ev->origin, ev->serial
        )) {
        wlr_seat_start_pointer_drag(server.seat, ev->drag, ev->serial);
    } else {
        wlr_data_source_destroy(ev->drag->source);
    }
}

static void handle_request_set_cursor(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_seat_pointer_request_set_cursor_event *ev = data;
    if (server.seat->pointer_state.focused_client == ev->seat_client) {
        wlr_cursor_set_surface(
            server.cursor, ev->surface, ev->hotspot_x, ev->hotspot_y
        );
    }
}
