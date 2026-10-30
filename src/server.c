#include "server.h"

#include "activation.h"
#include "background.h"
#include "config.h"
#include "cursor.h"
#include "input.h"
#include "layers.h"
#include "layouts/layout.h"
#include "log.h"
#include "manager.h"
#include "output.h"
#include "popup.h"
#include "seat.h"
#include "session.h"
#include "space.h"
#include "window.h"
#ifdef WALLPAPER
#include "wallpaper.h"
#endif

#include <scenefx/render/fx_renderer/fx_renderer.h>
#include <scenefx/types/fx/clipped_region.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-protocols/cursor-shape-v1-enum.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr-output-power-management-unstable-v1-protocol.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
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

static struct wl_list inhibitors;

struct Inhibitor {
    struct wlr_idle_inhibitor_v1 *inhibitor;
    struct wl_listener destroy;
    struct wl_list link;
};

static void handle_new_output(struct wl_listener *l, void *data);
static void handle_new_input(struct wl_listener *l, void *data);
static void handle_new_xdg_toplevel(struct wl_listener *l, void *data);
static void handle_new_xdg_popup(struct wl_listener *l, void *data);
static void handle_new_xdg_decoration(struct wl_listener *l, void *data);

static void handle_request_set_selection(struct wl_listener *l, void *data);
static void
handle_request_set_primary_selection(struct wl_listener *l, void *data);

static void handle_request_start_drag(struct wl_listener *l, void *data);
static void handle_request_set_cursor(struct wl_listener *l, void *data);

static void handle_output_power_set_mode(struct wl_listener *l, void *data);
static void handle_new_idle_inhibitor(struct wl_listener *l, void *data);

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

    wl_list_init(&inhibitors);

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

    server.compositor = wlr_compositor_create(display, 6, server.renderer);
    if (server.compositor == NULL) {
        log_err("Failed to create compositor");
        return -1;
    }

    wlr_subcompositor_create(display);
    wlr_data_device_manager_create(display);
    wlr_data_control_manager_v1_create(display);
    wlr_fractional_scale_manager_v1_create(display, 1);
    wlr_primary_selection_v1_device_manager_create(display);
    wlr_viewporter_create(display);
    wlr_relative_pointer_manager_v1_create(display);
    wlr_pointer_constraints_v1_create(display);
    wlr_server_decoration_manager_create(display);
    wlr_screencopy_manager_v1_create(display);

    server.foreign_toplevel_list =
        wlr_ext_foreign_toplevel_list_v1_create(display, 1);
    server.foreign_toplevel_manager =
        wlr_foreign_toplevel_manager_v1_create(display);

    server.output_layout = wlr_output_layout_create(display);
    wlr_xdg_output_manager_v1_create(display, server.output_layout);

    server.scene = wlr_scene_create();
    server.scene_layout =
        wlr_scene_attach_output_layout(server.scene, server.output_layout);

    server.content_tree = wlr_scene_tree_create(&server.scene->tree);

    server.background_tree = wlr_scene_tree_create(server.content_tree);
    server.bottom_tree = wlr_scene_tree_create(server.content_tree);
    server.tile_tree = wlr_scene_tree_create(server.content_tree);
    server.float_tree = wlr_scene_tree_create(server.content_tree);
    server.top_tree = wlr_scene_tree_create(server.content_tree);
    server.fullscreen_tree = wlr_scene_tree_create(server.content_tree);
    server.popup_tree = wlr_scene_tree_create(&server.scene->tree);
    server.overlay_tree = wlr_scene_tree_create(server.content_tree);
    server.lock_tree = wlr_scene_tree_create(&server.scene->tree);

    wlr_scene_node_set_enabled(&server.lock_tree->node, false);

    server.xdg_shell = wlr_xdg_shell_create(display, 6);
    if (server.xdg_shell == NULL) {
        log_err("Failed to create xdg-shell");
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
        log_err("Failed to create seat");
        return -1;
    }

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    uint32_t cursor_size =
        config.cursor_size > 0 ? (uint32_t)config.cursor_size : 24;
    server.cursor_mgr = wlr_xcursor_manager_create(NULL, cursor_size);

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

    server.request_set_primary_selection.notify =
        handle_request_set_primary_selection;
    wl_signal_add(
        &server.seat->events.request_set_primary_selection,
        &server.request_set_primary_selection
    );

    server.request_start_drag.notify = handle_request_start_drag;

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

    if (!ext_workspace_init(display)) { log_warn("ext-workspace unavailable"); }

    server.layer_shell = wlr_layer_shell_v1_create(display, 4);
    if (server.layer_shell == NULL) {
        log_err("Failed to create layer-shell global");
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

    if (!session_lock_init(display)) log_warn("Session lock unavailable");

    server.output_power_manager = wlr_output_power_manager_v1_create(display);
    server.output_power_set_mode.notify = handle_output_power_set_mode;
    wl_signal_add(
        &server.output_power_manager->events.set_mode,
        &server.output_power_set_mode
    );

    server.idle_notifier = wlr_idle_notifier_v1_create(display);
    server.idle_inhibit_manager = wlr_idle_inhibit_v1_create(display);
    server.new_idle_inhibitor.notify = handle_new_idle_inhibitor;
    wl_signal_add(
        &server.idle_inhibit_manager->events.new_inhibitor,
        &server.new_idle_inhibitor
    );

    seat_init();

    server.xdg_activation = wlr_xdg_activation_v1_create(display);
    if (server.xdg_activation == NULL) {
        log_err("Failed to create xdg-activation global");
        return -1;
    }

    activation_init(server.xdg_activation);

    if (!wlr_backend_start(server.backend)) {
        log_err("Failed to start wlroots backend");
        return -1;
    }

    log_info("Initialization completed");

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
    wl_list_remove(&server.request_set_primary_selection.link);
    wl_list_remove(&server.request_start_drag.link);
    wl_list_remove(&server.request_set_cursor.link);
    wl_list_remove(&server.request_set_cursor_shape.link);

    wl_list_remove(&server.output_power_set_mode.link);
    wl_list_remove(&server.new_idle_inhibitor.link);

    activation_finish();
    session_lock_detach_listeners();
}

void server_finish(void) {
    struct Output *out, *tmp;
    wl_list_for_each_safe(out, tmp, &server.outputs, link) output_destroy(out);

#ifdef WALLPAPER
    wallpaper_finish();
#endif
    background_effect_finish();
    ext_workspace_finish();

    if (server.cursor_mgr) wlr_xcursor_manager_destroy(server.cursor_mgr);
    if (server.cursor) wlr_cursor_destroy(server.cursor);
    if (server.output_layout) wlr_output_layout_destroy(server.output_layout);
    if (server.scene) wlr_scene_node_destroy(&server.scene->tree.node);
}

struct NodeTag *node_tag_find(struct wlr_scene_node *node) {
    for (; node != NULL; node = node->parent ? &node->parent->node : NULL) {
        if (node->data != NULL) return node->data;
    }

    return NULL;
}

static bool inhibitor_surface_visible(struct wlr_surface *surface) {
    if (server.locked) return false;
    struct wlr_surface *root = wlr_surface_get_root_surface(surface);

    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->closed) continue;
        if (window_wlr_surface(w) == root) {
            if (!w->mapped || w->space_hidden) return false;

            int lx = 0, ly = 0;
            if (!wlr_scene_node_coords(&w->scene_tree->node, &lx, &ly)) {
                return false;
            }

            return true;
        }
    }

    struct LayerSurface *ls = layer_surface_from_wlr_surface(root);
    if (ls != NULL) {
        if (!ls->mapped) return false;
        int lx = 0, ly = 0;
        return wlr_scene_node_coords(&ls->scene_tree->node, &lx, &ly);
    }

    return true;
}

void idle_update_inhibit(void) {
    if (server.idle_notifier == NULL) return;

    bool inhibited = false;
    struct Inhibitor *i;
    wl_list_for_each(i, &inhibitors, link) {
        if (inhibitor_surface_visible(i->inhibitor->surface)) {
            inhibited = true;
            break;
        }
    }

    wlr_idle_notifier_v1_set_inhibited(server.idle_notifier, inhibited);
}

static void inhibitor_handle_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct Inhibitor *i = wl_container_of(l, i, destroy);

    wl_list_remove(&i->destroy.link);
    wl_list_remove(&i->link);
    free(i);

    idle_update_inhibit();
}

static void handle_new_idle_inhibitor(struct wl_listener *l, void *data) {
    (void)l;

    struct Inhibitor *i = calloc(1, sizeof(*i));
    if (i == NULL) return;

    i->inhibitor = data;
    i->destroy.notify = inhibitor_handle_destroy;
    wl_signal_add(&i->inhibitor->events.destroy, &i->destroy);
    wl_list_insert(&inhibitors, &i->link);

    idle_update_inhibit();
}

void server_notify_activity(void) {
    if (server.idle_notifier == NULL) return;
    wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);
}

static void handle_output_power_set_mode(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_output_power_v1_set_mode_event *ev = data;

    if (output_set_enabled(
            ev->output, ev->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON
        )) {
        wm_mark_layout_dirty();
    }
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

    window_create((struct wlr_xdg_toplevel *)data);
}

static void handle_new_xdg_popup(struct wl_listener *l, void *data) {
    (void)l;

    struct wlr_xdg_popup *popup = data;
    if (popup->parent == NULL) return;

    struct wlr_xdg_surface *parent_xdg =
        wlr_xdg_surface_try_from_wlr_surface(popup->parent);

    if (parent_xdg == NULL || parent_xdg->data == NULL) {
        log_warn("xdg popup has no parent scene tree (dismissing)");
        wlr_xdg_popup_destroy(popup);
        return;
    }

    struct wlr_scene_tree *parent_tree = parent_xdg->data;

    if (parent_xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
        struct Window *window = window_from_scene_node(&parent_tree->node);
        if (window == NULL || window->popup_tree == NULL) {
            log_warn("xdg popup parent window not found (dismissing)");
            wlr_xdg_popup_destroy(popup);
            return;
        }

        parent_tree = window->popup_tree;
    }

    popup_track(popup, parent_tree);
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

static void
handle_request_set_primary_selection(struct wl_listener *l, void *data) {
    (void)l;

    struct wlr_seat_request_set_primary_selection_event *ev = data;
    wlr_seat_set_primary_selection(server.seat, ev->source, ev->serial);
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
