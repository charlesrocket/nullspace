#include "server.h"

#include "activation.h"
#include "background.h"
#include "config.h"
#include "controller.h"
#include "cursor.h"
#include "decoration.h"
#include "icon.h"
#include "input.h"
#include "layer.h"
#include "layout/layout.h"
#include "log.h"
#include "manager.h"
#include "output.h"
#include "popup.h"
#include "seat.h"
#include "security.h"
#include "session.h"
#include "space.h"
#include "wallpaper.h"
#include "window.h"

#include <nullspace-controller-v1-protocol.h>
#include <scenefx/render/fx_renderer/fx_renderer.h>
#include <scenefx/types/fx/clipped_region.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/signal.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-protocols/cursor-shape-v1-enum.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr-output-power-management-unstable-v1-protocol.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_system_bell_v1.h>
#include <wlr/util/log.h>

#define LOG_TOPIC          "server"

#define MAX_SHM_POOL_BYTES (512u * 1024u * 1024u)

struct DeferredDestroy {
    struct wl_resource* resource; // ?NULL once the client destroyed it
    struct wl_listener destroy;
};

static void deferred_destroy_handle(struct wl_listener* l, void* data) {
    (void)data;

    struct DeferredDestroy* d = wl_container_of(l, d, destroy);
    wl_list_remove(&d->destroy.link);
    d->resource = NULL;
}

static void deferred_destroy_idle(void* data) {
    struct DeferredDestroy* d = data;
    if (d->resource != NULL) wl_resource_destroy(d->resource);
    free(d);
}

static void defer_resource_destroy(struct wl_resource* resource) {
    struct DeferredDestroy* d = calloc(1, sizeof(*d));
    if (d == NULL) {
        wl_resource_destroy(resource);
        return;
    }

    d->resource = resource;
    d->destroy.notify = deferred_destroy_handle;
    wl_resource_add_destroy_listener(resource, &d->destroy);
    wl_event_loop_add_idle(server.event_loop, deferred_destroy_idle, d);
}

struct Server server = {0};

static struct wl_list inhibitors;
static struct wl_list shortcuts_inhibitors;

struct Inhibitor {
    struct wlr_idle_inhibitor_v1* inhibitor;
    struct wl_listener destroy;
    struct wl_list link;
};

struct ShortcutInhibitor {
    struct wlr_keyboard_shortcuts_inhibitor_v1* wlr_inhibitor;
    struct wl_listener destroy;
    struct wl_list link; // shortcuts_inhibitors
};

struct DragContext {
    struct wl_listener destroy;
};

static void handle_new_output(struct wl_listener* l, void* data);
static void handle_new_input(struct wl_listener* l, void* data);
static void handle_new_xdg_toplevel(struct wl_listener* l, void* data);
static void handle_new_xdg_popup(struct wl_listener* l, void* data);
static void handle_new_xdg_decoration(struct wl_listener* l, void* data);

static void handle_request_set_selection(struct wl_listener* l, void* data);
static void handle_request_start_drag(struct wl_listener* l, void* data);
static void handle_start_drag(struct wl_listener* l, void* data);
static void handle_request_set_cursor(struct wl_listener* l, void* data);

static void handle_output_power_set_mode(struct wl_listener* l, void* data);
static void handle_new_idle_inhibitor(struct wl_listener* l, void* data);
static void handle_new_shortcuts_inhibitor(struct wl_listener* l, void* data);

static const char* cursor_shape_name(uint32_t shape) {
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

static void drag_context_destroy(struct wl_listener* l, void* data) {
    (void)data;

    struct DragContext* ctx = wl_container_of(l, ctx, destroy);
    wl_list_remove(&ctx->destroy.link);
    free(ctx);

    seat_refocus_after_drag();
}

static void handle_start_drag(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_drag* drag = data;

    if (drag->icon != NULL) {
        struct wlr_scene_tree* icon_tree =
            wlr_scene_drag_icon_create(server.overlay_tree, drag->icon);

        if (icon_tree != NULL) {
            wlr_scene_node_set_position(
                &icon_tree->node, (int32_t)server.cursor->x,
                (int32_t)server.cursor->y
            );

            drag->icon->data = icon_tree;
        }
    }

    struct DragContext* ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) return;

    ctx->destroy.notify = drag_context_destroy;
    wl_signal_add(&drag->events.destroy, &ctx->destroy);
}

static void handle_request_start_drag(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_seat_request_start_drag_event* ev = data;
    if (wlr_seat_validate_pointer_grab_serial(
            server.seat, ev->origin, ev->serial
        )) {
        wlr_seat_start_pointer_drag(server.seat, ev->drag, ev->serial);
    } else {
        wlr_data_source_destroy(ev->drag->source);
    }
}

static void handle_request_set_cursor_shape(struct wl_listener* l, void* data) {
    struct Server* s = wl_container_of(l, s, request_set_cursor_shape);
    struct wlr_cursor_shape_manager_v1_request_set_shape_event* ev = data;

    if (s->seat->pointer_state.focused_surface == NULL) return;
    if (s->seat->pointer_state.focused_client != ev->seat_client) return;
    if (cursor_is_hidden()) return;

    wlr_cursor_set_xcursor(
        s->cursor, s->cursor_mgr, cursor_shape_name(ev->shape)
    );
}

static void handle_request_set_cursor(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_seat_pointer_request_set_cursor_event* ev = data;

    // No surface focused, so no client owns the cursor.
    if (server.seat->pointer_state.focused_surface == NULL) return;
    if (server.seat->pointer_state.focused_client != ev->seat_client) return;
    if (cursor_is_hidden()) return;

    wlr_cursor_set_surface(
        server.cursor, ev->surface, ev->hotspot_x, ev->hotspot_y
    );
}

static void handle_request_set_selection(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_seat_request_set_selection_event* ev = data;
    struct wlr_seat_client* fc = server.seat->keyboard_state.focused_client;
    if (fc == NULL) return;
    if (!wlr_seat_client_validate_event_serial(fc, ev->serial)) return;

    wlr_seat_set_selection(server.seat, ev->source, ev->serial);
}

static void handle_relative_pointer_use(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_relative_pointer_v1* rp = data;
    if (security_check_use(wl_resource_get_client(rp->resource), PERM_CURSORPOS)
        != DEC_ALLOW) {
        defer_resource_destroy(rp->resource);
    }
}

static void handle_virtual_keyboard_use(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_virtual_keyboard_v1* vk = data;
    if (security_check_use(wl_resource_get_client(vk->resource), PERM_KEYBOARD)
        != DEC_ALLOW) {
        wl_resource_destroy(vk->resource);
    }
}

static void handle_virtual_pointer_use(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_virtual_pointer_v1_new_pointer_event* ev = data;
    struct wlr_virtual_pointer_v1* vp = ev->new_pointer;
    if (security_check_use(wl_resource_get_client(vp->resource), PERM_POINTER)
        != DEC_ALLOW) {
        wl_resource_destroy(vp->resource);
    }
}

static void handle_ext_data_control_use(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_ext_data_control_device_v1* dev = data;
    if (security_check_use(
            wl_resource_get_client(dev->resource), PERM_CLIPBOARD
        )
        != DEC_ALLOW) {
        wlr_ext_data_control_device_v1_destroy(dev);
    }
}

static void
handle_image_copy_capture_session_use(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_ext_image_copy_capture_session_v1* session = data;
    if (security_check_use(
            wl_resource_get_client(session->resource), PERM_SCREENCOPY
        )
        != DEC_ALLOW) {
        wl_resource_destroy(session->resource);
    }
}

static void handle_system_bell_ring(struct wl_listener* l, void* data) {
    (void)l;
    (void)data;

    wm_bell_ring();
}

int server_init(struct wl_display* display) {
    server.display = display;
    server.event_loop = wl_display_get_event_loop(display);

    wl_display_set_default_max_buffer_size(display, MAX_SHM_POOL_BYTES);

    wl_list_init(&inhibitors);
    wl_list_init(&shortcuts_inhibitors);

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

    server.security_context_manager =
        wlr_security_context_manager_v1_create(display);

    if (server.security_context_manager == NULL) {
        log_err("Failed to create security-context manager");
        return -1;
    }

    PROTECT(server.security_context_manager, PERM_NONE);
    wl_display_set_global_filter(display, security_filter_global, NULL);

    server.relative_pointer_manager =
        wlr_relative_pointer_manager_v1_create(display);
    if (server.relative_pointer_manager != NULL) {
        server.rel_pointer_use.notify = handle_relative_pointer_use;
        wl_signal_add(
            &server.relative_pointer_manager->events.new_relative_pointer,
            &server.rel_pointer_use
        );
    } else {
        log_warn("Relative pointer unavailable");
    }

    server.pointer_constraints = wlr_pointer_constraints_v1_create(display);
    if (server.pointer_constraints != NULL) {
        if (!cursor_constraints_init()) {
            log_warn("Pointer constraints unavailable");
        }
    }

    security_sandbox_add(server.compositor->global);

    {
        struct wlr_subcompositor* s = wlr_subcompositor_create(display);
        security_sandbox_add(s ? s->global : NULL);
    }

    wlr_primary_selection_v1_device_manager_create(display);

    {
        struct wlr_data_device_manager* m =
            wlr_data_device_manager_create(display);
        security_sandbox_add(m ? m->global : NULL);
    }

    server.ext_data_control_manager =
        wlr_ext_data_control_manager_v1_create(display, 1);
    if (server.ext_data_control_manager != NULL) {
        PROTECT(server.ext_data_control_manager, PERM_CLIPBOARD);

        server.ext_dc_use.notify = handle_ext_data_control_use;
        wl_signal_add(
            &server.ext_data_control_manager->events.new_device,
            &server.ext_dc_use
        );
    } else {
        log_warn("ext-data-control manager unavailable");
    }

    {
        struct wlr_fractional_scale_manager_v1* f =
            wlr_fractional_scale_manager_v1_create(display, 1);
        security_sandbox_add(f ? f->global : NULL);
    }
    {
        struct wlr_viewporter* v = wlr_viewporter_create(display);
        security_sandbox_add(v ? v->global : NULL);
    }

    struct wlr_single_pixel_buffer_manager_v1* single_pixel_mgr =
        wlr_single_pixel_buffer_manager_v1_create(display);
    if (single_pixel_mgr != NULL) {
        security_sandbox_add(single_pixel_mgr->global);
    } else {
        log_warn("Single-pixel-buffer-v1 unavailable");
    }

    struct wlr_server_decoration_manager* kde_deco =
        wlr_server_decoration_manager_create(display);
    if (kde_deco != NULL) {
        wlr_server_decoration_manager_set_default_mode(
            kde_deco, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER
        );
    } else {
        log_warn("Server-side decorations unavailable");
    }

    server.foreign_toplevel_list =
        wlr_ext_foreign_toplevel_list_v1_create(display, 1);
    PROTECT(server.foreign_toplevel_list, PERM_NONE);

    server.foreign_toplevel_manager =
        wlr_foreign_toplevel_manager_v1_create(display);
    PROTECT(server.foreign_toplevel_manager, PERM_NONE);

    server.output_layout = wlr_output_layout_create(display);
    if (server.output_layout == NULL) {
        log_err("Failed to create output layout");
        return -1;
    }

    server.xdg_output_manager =
        wlr_xdg_output_manager_v1_create(display, server.output_layout);
    if (server.xdg_output_manager == NULL) {
        log_err("Failed to create xdg-output manager");
        return -1;
    }

    server.scene = wlr_scene_create();
    if (server.scene == NULL) {
        log_err("Failed to create scene");
        return -1;
    }

    server.scene_layout =
        wlr_scene_attach_output_layout(server.scene, server.output_layout);
    if (server.scene_layout == NULL) {
        log_err("Failed to attach scene layout");
        return -1;
    }

    server.presentation = wlr_presentation_create(display, server.backend, 2);
    if (server.presentation != NULL) {
        security_sandbox_add(server.presentation->global);
    } else {
        log_warn("Presentation-time unavailable");
    }

    server.content_tree = wlr_scene_tree_create(&server.scene->tree);
    if (server.content_tree == NULL) {
        log_err("Failed to create content tree");
        return -1;
    }

    server.background_tree = wlr_scene_tree_create(server.content_tree);
    server.bottom_tree = wlr_scene_tree_create(server.content_tree);
    server.tile_tree = wlr_scene_tree_create(server.content_tree);
    server.float_tree = wlr_scene_tree_create(server.content_tree);
    server.top_tree = wlr_scene_tree_create(server.content_tree);
    server.fullscreen_tree = wlr_scene_tree_create(server.content_tree);
    server.popup_tree = wlr_scene_tree_create(&server.scene->tree);
    server.overlay_tree = wlr_scene_tree_create(server.content_tree);
    server.lock_tree = wlr_scene_tree_create(&server.scene->tree);

    server.overview_tree = wlr_scene_tree_create(&server.scene->tree);

    wlr_scene_node_set_enabled(&server.overview_tree->node, false);
    wlr_scene_node_set_enabled(&server.lock_tree->node, false);

    server.xdg_shell = wlr_xdg_shell_create(display, 6);
    if (server.xdg_shell == NULL) {
        log_err("Failed to create xdg-shell");
        return -1;
    }

    security_sandbox_add(server.xdg_shell->global);

    server.new_xdg_toplevel.notify = handle_new_xdg_toplevel;
    wl_signal_add(
        &server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel
    );

    server.new_xdg_popup.notify = handle_new_xdg_popup;
    wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

    server.xdg_decoration = wlr_xdg_decoration_manager_v1_create(display);
    if (server.xdg_decoration == NULL) {
        log_err("Failed to create xdg-decoration manager");
        return -1;
    }

    security_sandbox_add(server.xdg_decoration->global);

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

    security_sandbox_add(server.seat->global);

    server.cursor = wlr_cursor_create();
    if (server.cursor == NULL) {
        log_err("Failed to create cursor");
        return -1;
    }

    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    uint32_t cursor_size =
        config.cursor_size > 0 ? (uint32_t)config.cursor_size : 24;

    server.cursor_mgr = wlr_xcursor_manager_create(NULL, cursor_size);
    if (server.cursor_mgr == NULL) {
        log_err("Failed to create xcursor manager");
        return -1;
    }

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

    server.cursor_swipe_begin.notify = cursor_handle_swipe_begin;
    wl_signal_add(
        &server.cursor->events.swipe_begin, &server.cursor_swipe_begin
    );

    server.cursor_swipe_update.notify = cursor_handle_swipe_update;
    wl_signal_add(
        &server.cursor->events.swipe_update, &server.cursor_swipe_update
    );

    server.cursor_swipe_end.notify = cursor_handle_swipe_end;
    wl_signal_add(&server.cursor->events.swipe_end, &server.cursor_swipe_end);

    server.cursor_pinch_begin.notify = cursor_handle_pinch_begin;
    wl_signal_add(
        &server.cursor->events.pinch_begin, &server.cursor_pinch_begin
    );

    server.cursor_pinch_update.notify = cursor_handle_pinch_update;
    wl_signal_add(
        &server.cursor->events.pinch_update, &server.cursor_pinch_update
    );

    server.cursor_pinch_end.notify = cursor_handle_pinch_end;
    wl_signal_add(&server.cursor->events.pinch_end, &server.cursor_pinch_end);

    server.cursor_hold_begin.notify = cursor_handle_hold_begin;
    wl_signal_add(&server.cursor->events.hold_begin, &server.cursor_hold_begin);

    server.cursor_hold_end.notify = cursor_handle_hold_end;
    wl_signal_add(&server.cursor->events.hold_end, &server.cursor_hold_end);

    server.cursor_touch_down.notify = cursor_handle_touch_down;
    wl_signal_add(&server.cursor->events.touch_down, &server.cursor_touch_down);

    server.cursor_touch_up.notify = cursor_handle_touch_up;
    wl_signal_add(&server.cursor->events.touch_up, &server.cursor_touch_up);

    server.cursor_touch_motion.notify = cursor_handle_touch_motion;
    wl_signal_add(
        &server.cursor->events.touch_motion, &server.cursor_touch_motion
    );

    server.cursor_touch_frame.notify = cursor_handle_touch_frame;
    wl_signal_add(
        &server.cursor->events.touch_frame, &server.cursor_touch_frame
    );

    server.cursor_touch_cancel.notify = cursor_handle_touch_cancel;
    wl_signal_add(
        &server.cursor->events.touch_cancel, &server.cursor_touch_cancel
    );

    server.request_set_selection.notify = handle_request_set_selection;
    wl_signal_add(
        &server.seat->events.request_set_selection,
        &server.request_set_selection
    );

    server.request_start_drag.notify = handle_request_start_drag;
    wl_signal_add(
        &server.seat->events.request_start_drag, &server.request_start_drag
    );

    server.start_drag.notify = handle_start_drag;
    wl_signal_add(&server.seat->events.start_drag, &server.start_drag);

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

    if (!ext_workspace_init(display)) {
        log_warn("ext-workspace unavailable");
    } else {
        PROTECT(server.workspace_manager, PERM_NONE);
    }

    server.layer_shell = wlr_layer_shell_v1_create(display, 5);
    if (server.layer_shell == NULL) {
        log_err("Failed to create layer-shell global");
        return -1;
    }

    PROTECT(server.layer_shell, PERM_NONE);

    server.output_manager = wlr_output_manager_v1_create(display);
    PROTECT(server.output_manager, PERM_NONE);

    server.new_layer_surface.notify = layer_handle_new_surface;
    wl_signal_add(
        &server.layer_shell->events.new_surface, &server.new_layer_surface
    );

    if (!background_effect_init(display)) {
        log_warn("Background effect unavailable");
    }

    server.cursor_shape_manager =
        wlr_cursor_shape_manager_v1_create(display, 1);
    if (server.cursor_shape_manager == NULL) {
        log_err("Failed to create cursor-shape manager");
        return -1;
    }

    security_sandbox_add(server.cursor_shape_manager->global);

    server.request_set_cursor_shape.notify = handle_request_set_cursor_shape;
    wl_signal_add(
        &server.cursor_shape_manager->events.request_set_shape,
        &server.request_set_cursor_shape
    );

    server.pointer_gestures = wlr_pointer_gestures_v1_create(display);
    if (server.pointer_gestures != NULL) {
        PROTECT(server.pointer_gestures, PERM_NONE);
    } else {
        log_warn("Pointer gestures unavailable");
    }

    server.content_type_manager =
        wlr_content_type_manager_v1_create(display, 1);
    if (server.content_type_manager == NULL) {
        log_warn("Content type unavailable");
    }

    {
        struct wlr_text_input_manager_v3* t =
            wlr_text_input_manager_v3_create(display);
        security_sandbox_add(t ? t->global : NULL);
    }

    if (!session_lock_init(display)) {
        log_warn("Session lock unavailable");
    } else {
        struct wlr_session_lock_manager_v1* lock_mgr =
            session_lock_get_manager();
        if (lock_mgr != NULL) { PROTECT(lock_mgr, PERM_NONE); }
    }

    server.output_power_manager = wlr_output_power_manager_v1_create(display);
    if (server.output_power_manager == NULL) {
        log_err("Failed to create output-power manager");
        return -1;
    }

    PROTECT(server.output_power_manager, PERM_NONE);
    server.output_power_set_mode.notify = handle_output_power_set_mode;
    wl_signal_add(
        &server.output_power_manager->events.set_mode,
        &server.output_power_set_mode
    );

    server.ext_image_copy_capture_manager =
        wlr_ext_image_copy_capture_manager_v1_create(display, 1);
    PROTECT(server.ext_image_copy_capture_manager, PERM_SCREENCOPY);
    if (server.ext_image_copy_capture_manager != NULL) {
        server.ext_capture_session_use.notify =
            handle_image_copy_capture_session_use;
        wl_signal_add(
            &server.ext_image_copy_capture_manager->events.new_session,
            &server.ext_capture_session_use
        );
    }

    server.ext_output_image_capture_source_manager =
        wlr_ext_output_image_capture_source_manager_v1_create(display, 1);
    if (server.ext_output_image_capture_source_manager != NULL) {
        PROTECT(
            server.ext_output_image_capture_source_manager, PERM_SCREENCOPY
        );
    } else {
        log_warn("Output image capture source manager unavailable");
    }

    server.virtual_keyboard_manager =
        wlr_virtual_keyboard_manager_v1_create(display);

    if (server.virtual_keyboard_manager != NULL) {
        PROTECT(server.virtual_keyboard_manager, PERM_KEYBOARD);
        server.vk_use.notify = handle_virtual_keyboard_use;
        wl_signal_add(
            &server.virtual_keyboard_manager->events.new_virtual_keyboard,
            &server.vk_use
        );
    } else {
        log_warn("Virtual keyboard unavailable");
    }

    server.virtual_pointer_manager =
        wlr_virtual_pointer_manager_v1_create(display);
    if (server.virtual_pointer_manager != NULL) {
        PROTECT(server.virtual_pointer_manager, PERM_POINTER);
        server.vp_use.notify = handle_virtual_pointer_use;
        wl_signal_add(
            &server.virtual_pointer_manager->events.new_virtual_pointer,
            &server.vp_use
        );
    } else {
        log_warn("Virtual pointer unavailable");
    }

    server.keyboard_shortcuts_inhibit =
        wlr_keyboard_shortcuts_inhibit_v1_create(display);
    if (server.keyboard_shortcuts_inhibit == NULL) {
        log_warn("Keyboard shortcuts inhibit unavailable");
    } else {
        PROTECT(server.keyboard_shortcuts_inhibit, PERM_NONE);

        server.new_shortcuts_inhibitor.notify = handle_new_shortcuts_inhibitor;
        wl_signal_add(
            &server.keyboard_shortcuts_inhibit->events.new_inhibitor,
            &server.new_shortcuts_inhibitor
        );
    }

    server.tearing_control = wlr_tearing_control_manager_v1_create(display, 1);
    if (server.tearing_control != NULL) {
        security_sandbox_add(server.tearing_control->global);
    } else {
        log_warn("Tearing control unavailable");
    }

    server.gamma_control_manager = wlr_gamma_control_manager_v1_create(display);
    PROTECT(server.gamma_control_manager, PERM_NONE);

    server.idle_notifier = wlr_idle_notifier_v1_create(display);
    PROTECT(server.idle_notifier, PERM_NONE);

    server.idle_inhibit_manager = wlr_idle_inhibit_v1_create(display);
    if (server.idle_inhibit_manager == NULL) {
        log_err("Failed to create idle-inhibit manager");
        return -1;
    }

    PROTECT(server.idle_inhibit_manager, PERM_NONE);
    server.new_idle_inhibitor.notify = handle_new_idle_inhibitor;
    wl_signal_add(
        &server.idle_inhibit_manager->events.new_inhibitor,
        &server.new_idle_inhibitor
    );

    if (wm_bell_init()) {
        server.system_bell = wlr_xdg_system_bell_v1_create(display, 1);
        if (server.system_bell != NULL) {
            PROTECT(server.system_bell, PERM_NONE);
            server.system_bell_ring.notify = handle_system_bell_ring;
            wl_signal_add(
                &server.system_bell->events.ring, &server.system_bell_ring
            );
        }
    }

    if (server.system_bell == NULL) log_warn("System bell unavailable");

    server.nullspace_controller = controller_create(display);
    PROTECT(server.nullspace_controller, PERM_NONE);
    seat_init();
    if (!icon_manager_init(display)) log_warn("Toplevel icon unavailable");

    server.xdg_activation = wlr_xdg_activation_v1_create(display);
    if (server.xdg_activation == NULL) {
        log_err("Failed to create xdg-activation global");
        return -1;
    }

    PROTECT(server.xdg_activation, PERM_NONE);

    deco_anim_init(wl_display_get_event_loop(server.display));
    cursor_hide_init(wl_display_get_event_loop(server.display));
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

    wl_list_remove(&server.cursor_swipe_begin.link);
    wl_list_remove(&server.cursor_swipe_update.link);
    wl_list_remove(&server.cursor_swipe_end.link);
    wl_list_remove(&server.cursor_pinch_begin.link);
    wl_list_remove(&server.cursor_pinch_update.link);
    wl_list_remove(&server.cursor_pinch_end.link);
    wl_list_remove(&server.cursor_hold_begin.link);
    wl_list_remove(&server.cursor_hold_end.link);
    wl_list_remove(&server.cursor_motion.link);
    wl_list_remove(&server.cursor_motion_absolute.link);
    wl_list_remove(&server.cursor_button.link);
    wl_list_remove(&server.cursor_axis.link);
    wl_list_remove(&server.cursor_frame.link);
    wl_list_remove(&server.cursor_touch_down.link);
    wl_list_remove(&server.cursor_touch_up.link);
    wl_list_remove(&server.cursor_touch_motion.link);
    wl_list_remove(&server.cursor_touch_frame.link);
    wl_list_remove(&server.cursor_touch_cancel.link);

    wl_list_remove(&server.request_set_selection.link);
    wl_list_remove(&server.request_start_drag.link);
    wl_list_remove(&server.start_drag.link);
    wl_list_remove(&server.request_set_cursor.link);
    wl_list_remove(&server.request_set_cursor_shape.link);

    wl_list_remove(&server.output_power_set_mode.link);
    wl_list_remove(&server.new_idle_inhibitor.link);

    if (server.rel_pointer_use.link.prev != NULL) {
        wl_list_remove(&server.rel_pointer_use.link);
    }

    if (server.vk_use.link.prev != NULL) {
        wl_list_remove(&server.vk_use.link);
    }

    if (server.vp_use.link.prev != NULL) {
        wl_list_remove(&server.vp_use.link);
    }

    if (server.ext_dc_use.link.prev != NULL) {
        wl_list_remove(&server.ext_dc_use.link);
    }

    if (server.ext_capture_session_use.link.prev != NULL) {
        wl_list_remove(&server.ext_capture_session_use.link);
    }

    if (server.keyboard_shortcuts_inhibit != NULL) {
        wl_list_remove(&server.new_shortcuts_inhibitor.link);
    }

    activation_finish();
    cursor_constraints_finish();
    session_lock_detach_listeners();
}

void server_finish(void) {
    struct Output *out, *tmp;
    wl_list_for_each_safe(out, tmp, &server.outputs, link) output_destroy(out);

    deco_anim_finish();
    cursor_hide_finish();
    security_permission_finish();
    seat_finish();
    icon_manager_finish();
    wallpaper_finish();
    background_effect_finish();
    ext_workspace_finish();
    controller_destroy(server.nullspace_controller);

    if (server.cursor_mgr) wlr_xcursor_manager_destroy(server.cursor_mgr);
    if (server.cursor) wlr_cursor_destroy(server.cursor);
    if (server.output_layout) wlr_output_layout_destroy(server.output_layout);
    if (server.scene) wlr_scene_node_destroy(&server.scene->tree.node);
}

struct NodeTag* node_tag_find(const struct wlr_scene_node* node) {
    for (; node != NULL; node = node->parent ? &node->parent->node : NULL) {
        if (node->data != NULL) return node->data;
    }

    return NULL;
}

static bool inhibitor_surface_visible(struct wlr_surface* surface) {
    if (server.locked) return false;
    struct wlr_surface* root = wlr_surface_get_root_surface(surface);

    struct Window* w;
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

    struct LayerSurface* ls = layer_surface_from_wlr_surface(root);
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
    struct Inhibitor* i;
    wl_list_for_each(i, &inhibitors, link) {
        if (inhibitor_surface_visible(i->inhibitor->surface)) {
            inhibited = true;
            break;
        }
    }

    wlr_idle_notifier_v1_set_inhibited(server.idle_notifier, inhibited);
}

static void inhibitor_handle_destroy(struct wl_listener* l, void* data) {
    (void)data;

    struct Inhibitor* i = wl_container_of(l, i, destroy);

    wl_list_remove(&i->destroy.link);
    wl_list_remove(&i->link);
    free(i);

    idle_update_inhibit();
}

static void handle_new_idle_inhibitor(struct wl_listener* l, void* data) {
    (void)l;

    struct Inhibitor* i = calloc(1, sizeof(*i));
    if (i == NULL) return;

    i->inhibitor = data;
    i->destroy.notify = inhibitor_handle_destroy;
    wl_signal_add(&i->inhibitor->events.destroy, &i->destroy);
    wl_list_insert(&inhibitors, &i->link);

    idle_update_inhibit();
}

static void shortcuts_inhibitor_destroy(struct wl_listener* l, void* data) {
    (void)data;

    struct ShortcutInhibitor* si = wl_container_of(l, si, destroy);
    wl_list_remove(&si->destroy.link);
    wl_list_remove(&si->link);
    free(si);
}

static void handle_new_shortcuts_inhibitor(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_keyboard_shortcuts_inhibitor_v1* inhibitor = data;

    struct ShortcutInhibitor* si = calloc(1, sizeof(*si));
    if (si == NULL) {
        wl_resource_destroy(inhibitor->resource);
        return;
    }

    si->wlr_inhibitor = inhibitor;

    si->destroy.notify = shortcuts_inhibitor_destroy;
    wl_signal_add(&inhibitor->events.destroy, &si->destroy);

    wl_list_insert(&shortcuts_inhibitors, &si->link);

    wlr_keyboard_shortcuts_inhibitor_v1_activate(inhibitor);
}

bool server_shortcuts_inhibited(void) {
    struct wlr_surface* focused = server.seat->keyboard_state.focused_surface;
    if (focused == NULL) return false;

    struct ShortcutInhibitor* si;
    wl_list_for_each(si, &shortcuts_inhibitors, link) {
        if (si->wlr_inhibitor->active
            && si->wlr_inhibitor->surface == focused) {
            return true;
        }
    }

    return false;
}

void server_notify_activity(void) {
    if (server.idle_notifier == NULL) return;
    wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);
}

static void handle_output_power_set_mode(struct wl_listener* l, void* data) {
    (void)l;
    struct wlr_output_power_v1_set_mode_event* ev = data;

    if (output_set_enabled(
            ev->output, ev->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON
        )) {
        wm_mark_layout_dirty();
        output_update_manager_config();
    }
}

static void handle_new_output(struct wl_listener* l, void* data) {
    (void)l;

    output_create((struct wlr_output*)data);
}

static void handle_new_input(struct wl_listener* l, void* data) {
    (void)l;

    seat_handle_new_input((struct wlr_input_device*)data);
}

static void handle_new_xdg_toplevel(struct wl_listener* l, void* data) {
    (void)l;

    window_create((struct wlr_xdg_toplevel*)data);
}

static void handle_new_xdg_popup(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_xdg_popup* popup = data;
    if (popup->parent == NULL) return;

    struct wlr_xdg_surface* parent_xdg =
        wlr_xdg_surface_try_from_wlr_surface(popup->parent);

    if (parent_xdg == NULL || parent_xdg->data == NULL) {
        log_warn("xdg popup has no parent scene tree (dismissing)");
        wlr_xdg_popup_destroy(popup);
        return;
    }

    struct wlr_scene_tree* parent_tree = parent_xdg->data;

    if (parent_xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
        struct Window* window = window_from_scene_node(&parent_tree->node);
        if (window == NULL || window->popup_tree == NULL) {
            log_warn("xdg popup parent window not found (dismissing)");
            wlr_xdg_popup_destroy(popup);
            return;
        }

        parent_tree = window->popup_tree;
    }

    popup_track(popup, parent_tree);
}

struct XdgDecoration {
    struct wlr_xdg_toplevel_decoration_v1* decoration;
    struct wl_listener request_mode;
    struct wl_listener surface_commit;
    struct wl_listener destroy;
    bool commit_listening;
};

static void xdg_decoration_apply(struct XdgDecoration* d) {
    if (!d->decoration->toplevel->base->initialized) return;

    wlr_xdg_toplevel_decoration_v1_set_mode(
        d->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
    );
}

static void xdg_decoration_handle_commit(struct wl_listener* l, void* data) {
    (void)data;

    struct XdgDecoration* d = wl_container_of(l, d, surface_commit);
    if (!d->decoration->toplevel->base->initialized) return;

    wl_list_remove(&d->surface_commit.link);
    d->commit_listening = false;
    xdg_decoration_apply(d);
}

static void
xdg_decoration_handle_request_mode(struct wl_listener* l, void* data) {
    (void)data;

    struct XdgDecoration* d = wl_container_of(l, d, request_mode);

    // SSD-only policy.
    // If the surface is not initialized yet, the commit hook applies it later.
    xdg_decoration_apply(d);
}

static void xdg_decoration_handle_destroy(struct wl_listener* l, void* data) {
    (void)data;

    struct XdgDecoration* d = wl_container_of(l, d, destroy);

    if (d->commit_listening) wl_list_remove(&d->surface_commit.link);
    wl_list_remove(&d->request_mode.link);
    wl_list_remove(&d->destroy.link);
    free(d);
}

static void handle_new_xdg_decoration(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_xdg_toplevel_decoration_v1* deco = data;

    struct XdgDecoration* d = calloc(1, sizeof(*d));
    if (d == NULL) {
        log_err("xdg-decoration: OOM (mode will not be enforced)");
        return;
    }

    d->decoration = deco;

    d->request_mode.notify = xdg_decoration_handle_request_mode;
    wl_signal_add(&deco->events.request_mode, &d->request_mode);
    d->destroy.notify = xdg_decoration_handle_destroy;
    wl_signal_add(&deco->events.destroy, &d->destroy);

    if (deco->toplevel->base->initialized) {
        xdg_decoration_apply(d);
    } else {
        d->surface_commit.notify = xdg_decoration_handle_commit;
        wl_signal_add(
            &deco->toplevel->base->surface->events.commit, &d->surface_commit
        );

        d->commit_listening = true;
    }
}
