#include "nullspace.h"

#include "wallpaper.h"

#include <dev/evdev/input-event-codes.h>
#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

static void output_handle_removed(void *data, struct river_output_v1 *obj) {
    struct Output *output = data;
    output->removed = true;
}

// Ignored events
static void output_handle_wl_output(
    void *data, struct river_output_v1 *obj, uint32_t name
) {}
static void output_handle_position(
    void *data, struct river_output_v1 *obj, int32_t x, int32_t y
) {}

static void output_handle_dimensions(
    void *data, struct river_output_v1 *obj, int32_t width, int32_t height
) {
    struct Output *output = data;
    output->width = width;
    output->height = height;

    if (output->wallpaper != NULL) {
        wallpaper_output_set_dimensions(output->wallpaper, width, height);
    }
}

const struct river_output_v1_listener river_output_listener = {
    .removed = output_handle_removed,
    .wl_output = output_handle_wl_output,
    .position = output_handle_position,
    .dimensions = output_handle_dimensions,
};

static void output_maybe_destroy(struct Output *output) {
    if (!output->removed) { return; }
    if (output->wallpaper != NULL) {
        wallpaper_output_destroy(output->wallpaper);
    }

    river_output_v1_destroy(output->obj);
    wl_list_remove(&output->link);
    free(output);
}

static struct Output *tiling_output(void) {
    struct Output *output;
    wl_list_for_each(output, &wm.outputs, link) {
        if (output->removed) { continue; }
        if (output->width <= 0 || output->height <= 0) { continue; }

        return output;
    }

    return NULL;
}

static void window_handle_closed(void *data, struct river_window_v1 *obj) {
    struct Window *window = data;
    window->closed = true;
}

static void window_handle_dimensions(
    void *data, struct river_window_v1 *obj, int32_t width, int32_t height
) {
    struct Window *window = data;
    window->width = width;
    window->height = height;
}

static void window_handle_pointer_move_requested(
    void *data, struct river_window_v1 *obj, struct river_seat_v1 *river_seat
) {
    struct Window *window = data;
    window->pointer_move_requested = river_seat_v1_get_user_data(river_seat);
}

static void window_handle_pointer_resize_requested(
    void *data, struct river_window_v1 *obj, struct river_seat_v1 *river_seat,
    uint32_t edges
) {
    struct Window *window = data;

    window->pointer_resize_requested = river_seat_v1_get_user_data(river_seat);
    window->pointer_resize_requested_edges = edges;
}

// Ignored events
static void window_handle_dimensions_hint(
    void *data, struct river_window_v1 *obj, int32_t min_width,
    int32_t min_height, int32_t max_width, int32_t max_height
) {}

static void window_handle_app_id(
    void *data, struct river_window_v1 *obj, const char *app_id
) {}

static void window_handle_title(
    void *data, struct river_window_v1 *obj, const char *title
) {}

static void window_handle_parent(
    void *data, struct river_window_v1 *obj, struct river_window_v1 *parent
) {}

static void window_handle_decoration_hint(
    void *data, struct river_window_v1 *obj, uint32_t hint
) {}

static void window_handle_show_window_menu_requested(
    void *data, struct river_window_v1 *obj, int32_t x, int32_t y
) {}

static void
window_handle_maximize_requested(void *data, struct river_window_v1 *obj) {}

static void
window_handle_unmaximize_requested(void *data, struct river_window_v1 *obj) {}

static void window_handle_fullscreen_requested(
    void *data, struct river_window_v1 *obj,
    struct river_output_v1 *river_output
) {}

static void window_handle_exit_fullscreen_requested(
    void *data, struct river_window_v1 *obj
) {}

static void
window_handle_minimize_requested(void *data, struct river_window_v1 *obj) {}

static void window_handle_unreliable_pid(
    void *data, struct river_window_v1 *obj, int32_t unreliable_pid
) {}

static void window_handle_presentation_hint(
    void *data, struct river_window_v1 *obj, uint32_t hint
) {}

static void window_handle_identifier(
    void *data, struct river_window_v1 *obj, const char *identifier
) {}

const struct river_window_v1_listener river_window_listener = {
    .closed = window_handle_closed,
    .dimensions_hint = window_handle_dimensions_hint,
    .dimensions = window_handle_dimensions,
    .app_id = window_handle_app_id,
    .title = window_handle_title,
    .parent = window_handle_parent,
    .decoration_hint = window_handle_decoration_hint,
    .pointer_move_requested = window_handle_pointer_move_requested,
    .pointer_resize_requested = window_handle_pointer_resize_requested,
    .show_window_menu_requested = window_handle_show_window_menu_requested,
    .maximize_requested = window_handle_maximize_requested,
    .unmaximize_requested = window_handle_unmaximize_requested,
    .fullscreen_requested = window_handle_fullscreen_requested,
    .exit_fullscreen_requested = window_handle_exit_fullscreen_requested,
    .minimize_requested = window_handle_minimize_requested,
    .unreliable_pid = window_handle_unreliable_pid,
    .presentation_hint = window_handle_presentation_hint,
    .identifier = window_handle_identifier,
};

static void window_maybe_destroy(struct Window *window) {
    if (!window->closed) { return; }

    struct Seat *seat;

    wl_list_for_each(seat, &wm.seats, link) {
        if (seat->focused == window) { seat->focused = NULL; }

        if (seat->op_window == window) {
            river_seat_v1_op_end(seat->obj);
            seat->op = SEAT_OP_NONE;
            seat->op_window = NULL;
        }
    }

    river_window_v1_destroy(window->obj);
    wl_list_remove(&window->link);
    free(window);
}

static void window_set_position(struct Window *window, int32_t x, int32_t y) {
    river_node_v1_set_position(window->node, x, y);
    window->x = x;
    window->y = y;
}

static void seat_pointer_move(struct Seat *seat, struct Window *window);
static void
seat_pointer_resize(struct Seat *seat, struct Window *window, uint32_t edges);

static void window_manage(struct Window *window) {
    if (window->new) {
        window->new = false;
        window_set_position(window, 0, 0);
        river_window_v1_propose_dimensions(window->obj, 0, 0);
    }

    if (!window->decoration_state_set
        || window->decoration_state != wm.layout) {
        if (wm.layout == LAYOUT_TILING) {
            river_window_v1_use_ssd(window->obj);
            river_window_v1_set_tiled(
                window->obj,
                RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM
                    | RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT
            );
        } else {
            river_window_v1_use_csd(window->obj);
            river_window_v1_set_tiled(window->obj, RIVER_WINDOW_V1_EDGES_NONE);
        }

        window->decoration_state = wm.layout;
        window->decoration_state_set = true;
    }

    if (wm.layout == LAYOUT_TILING) {
        window->pointer_move_requested = NULL;
        window->pointer_resize_requested = NULL;
        return;
    }

    if (window->pointer_move_requested != NULL) {
        seat_pointer_move(window->pointer_move_requested, window);
        window->pointer_move_requested = NULL;
    }

    if (window->pointer_resize_requested != NULL) {
        seat_pointer_resize(
            window->pointer_resize_requested, window,
            window->pointer_resize_requested_edges
        );

        window->pointer_resize_requested = NULL;
    }
}

static void distribute_evenly(
    int32_t total, int32_t n, int32_t gap, int32_t *sizes, int32_t *starts
) {
    int32_t reserved = gap * (n + 1);
    int32_t available = total - reserved;

    if (available < n) { available = n; } // keep sizes >= 1px

    int32_t base = available / n;
    int32_t remainder = available % n;
    int32_t pos = gap;

    for (int32_t i = 0; i < n; i++) {
        int32_t size = base + (i < remainder ? 1 : 0);
        sizes[i] = size;
        starts[i] = pos;
        pos += size + gap;
    }
}

static void layout_tiled_apply(void) {
    struct Output *output = tiling_output();
    if (output == NULL) { return; }

    int32_t count = 0;
    struct Window *window;
    wl_list_for_each(window, &wm.windows, link) { count++; }

    if (count == 0) { return; }

    int32_t out_w = output->width;
    int32_t out_h = output->height;

    // cols = ceil(sqrt(count)), rows = ceil(count / cols)
    int32_t cols = 1;
    while (cols * cols < count) { cols++; }
    int32_t rows = (count + cols - 1) / cols;

    int32_t row_heights[rows];
    int32_t row_y[rows];
    distribute_evenly(out_h, rows, TILED_GAP, row_heights, row_y);

    int32_t index = 0; // 0-based position of the current window overall

    wl_list_for_each(window, &wm.windows, link) {
        int32_t row = index / cols;
        int32_t col = index % cols;
        int32_t row_start = row * cols;
        int32_t row_cols = count - row_start < cols ? count - row_start : cols;
        int32_t col_widths[row_cols];
        int32_t col_x[row_cols];

        distribute_evenly(out_w, row_cols, TILED_GAP, col_widths, col_x);

        int32_t w = col_widths[col];
        int32_t h = row_heights[row];
        int32_t x = col_x[col];
        int32_t y = row_y[row];

        window_set_position(window, x, y);
        river_window_v1_propose_dimensions(window->obj, w, h);

        index++;
    }
}

static void
xkb_binding_handle_pressed(void *data, struct river_xkb_binding_v1 *obj) {
    struct XkbBinding *binding = data;
    binding->seat->pending_action = binding->action;
}

static void
xkb_binding_handle_released(void *data, struct river_xkb_binding_v1 *obj) {}

const struct river_xkb_binding_v1_listener river_xkb_binding_listener = {
    .pressed = xkb_binding_handle_pressed,
    .released = xkb_binding_handle_released,
};

static void xkb_binding_destroy(struct XkbBinding *binding) {
    river_xkb_binding_v1_destroy(binding->obj);
    wl_list_remove(&binding->link);
    free(binding);
}

static void xkb_binding_create(
    struct Seat *seat, uint32_t mods, xkb_keysym_t keysym, enum Action action
) {
    struct XkbBinding *binding = calloc(1, sizeof(struct XkbBinding));
    binding->obj = river_xkb_bindings_v1_get_xkb_binding(
        xkb_bindings_v1, seat->obj, keysym, mods
    );

    binding->seat = seat;
    binding->action = action;

    river_xkb_binding_v1_add_listener(
        binding->obj, &river_xkb_binding_listener, binding
    );

    river_xkb_binding_v1_enable(binding->obj);

    wl_list_insert(seat->xkb_bindings.prev, &binding->link);
}

static void pointer_binding_handle_pressed(
    void *data, struct river_pointer_binding_v1 *obj
) {
    struct PointerBinding *binding = data;
    binding->seat->pending_action = binding->action;
}

static void pointer_binding_handle_released(
    void *data, struct river_pointer_binding_v1 *obj
) {}

const struct river_pointer_binding_v1_listener river_pointer_binding_listener =
    {
        .pressed = pointer_binding_handle_pressed,
        .released = pointer_binding_handle_released,
};

static void pointer_binding_destroy(struct PointerBinding *binding) {
    river_pointer_binding_v1_destroy(binding->obj);
    wl_list_remove(&binding->link);
    free(binding);
}

static void pointer_binding_create(
    struct Seat *seat, uint32_t mods, uint32_t button, enum Action action
) {
    struct PointerBinding *binding = calloc(1, sizeof(struct PointerBinding));
    binding->obj = river_seat_v1_get_pointer_binding(seat->obj, button, mods);
    binding->seat = seat;
    binding->action = action;

    river_pointer_binding_v1_add_listener(
        binding->obj, &river_pointer_binding_listener, binding
    );

    river_pointer_binding_v1_enable(binding->obj);

    wl_list_insert(seat->pointer_bindings.prev, &binding->link);
}

static void seat_handle_removed(void *data, struct river_seat_v1 *obj) {
    struct Seat *seat = data;
    seat->removed = true;
}

static void seat_handle_pointer_enter(
    void *data, struct river_seat_v1 *obj, struct river_window_v1 *river_window
) {
    struct Seat *seat = data;
    seat->hovered = river_window_v1_get_user_data(river_window);
}

static void seat_handle_pointer_leave(void *data, struct river_seat_v1 *obj) {
    struct Seat *seat = data;
    seat->hovered = NULL;
}

static void seat_handle_window_interaction(
    void *data, struct river_seat_v1 *obj, struct river_window_v1 *river_window
) {
    struct Seat *seat = data;
    seat->interacted = river_window_v1_get_user_data(river_window);
}

static void seat_handle_op_delta(
    void *data, struct river_seat_v1 *obj, int32_t dx, int32_t dy
) {
    struct Seat *seat = data;
    seat->op_dx = dx;
    seat->op_dy = dy;
}

static void seat_handle_op_release(void *data, struct river_seat_v1 *obj) {
    struct Seat *seat = data;
    seat->op_release = true;
}

// Ignored events
static void
seat_handle_wl_seat(void *data, struct river_seat_v1 *obj, uint32_t id) {}
static void seat_handle_shell_surface_interaction(
    void *data, struct river_seat_v1 *obj,
    struct river_shell_surface_v1 *river_shell_surface
) {}

static void seat_handle_pointer_position(
    void *data, struct river_seat_v1 *obj, int32_t x, int32_t y
) {}

const struct river_seat_v1_listener river_seat_listener = {
    .removed = seat_handle_removed,
    .wl_seat = seat_handle_wl_seat,
    .pointer_enter = seat_handle_pointer_enter,
    .pointer_leave = seat_handle_pointer_leave,
    .window_interaction = seat_handle_window_interaction,
    .shell_surface_interaction = seat_handle_shell_surface_interaction,
    .op_delta = seat_handle_op_delta,
    .op_release = seat_handle_op_release,
    .pointer_position = seat_handle_pointer_position,
};

static void seat_maybe_destroy(struct Seat *seat) {
    if (!seat->removed) { return; }

    struct XkbBinding *xkb_binding, *xkb_binding_tmp;

    wl_list_for_each_safe(
        xkb_binding, xkb_binding_tmp, &seat->xkb_bindings, link
    ) {
        xkb_binding_destroy(xkb_binding);
    }

    struct PointerBinding *pointer_binding, *pointer_binding_tmp;

    wl_list_for_each_safe(
        pointer_binding, pointer_binding_tmp, &seat->pointer_bindings, link
    ) {
        pointer_binding_destroy(pointer_binding);
    }

    river_seat_v1_destroy(seat->obj);
    wl_list_remove(&seat->link);
    free(seat);
}

static void seat_focus(struct Seat *seat, struct Window *window) {
    // Focus the top window (if any) when there is no explicit target
    if (window == NULL && !wl_list_empty(&wm.windows)) {
        window = wl_container_of(wm.windows.prev, window, link);
    }

    if (seat->focused == window) { return; }

    if (window != NULL) {
        river_seat_v1_focus_window(seat->obj, window->obj);
        river_node_v1_place_top(window->node);
        wl_list_remove(&window->link);
        wl_list_insert(wm.windows.prev, &window->link);
    } else {
        river_seat_v1_clear_focus(seat->obj);
    }

    seat->focused = window;
}

static void seat_pointer_move(struct Seat *seat, struct Window *window) {
    seat_focus(seat, window);
    river_seat_v1_op_start_pointer(seat->obj);

    seat->op = SEAT_OP_MOVE;
    seat->op_window = window;
    seat->op_start_x = window->x;
    seat->op_start_y = window->y;
    seat->op_dx = 0;
    seat->op_dy = 0;
}

static void
seat_pointer_resize(struct Seat *seat, struct Window *window, uint32_t edges) {
    seat_focus(seat, window);
    river_window_v1_inform_resize_start(window->obj);
    river_seat_v1_op_start_pointer(seat->obj);

    seat->op = SEAT_OP_RESIZE;
    seat->op_window = window;
    seat->op_edges = edges;
    seat->op_start_x = window->x;
    seat->op_start_y = window->y;
    seat->op_start_width = window->width;
    seat->op_start_height = window->height;
    seat->op_dx = 0;
    seat->op_dy = 0;
}

static void seat_action(struct Seat *seat, enum Action action) {
    switch (action) {
        case ACTION_NONE: break;
        case ACTION_SPAWN_FOOT:
            if (fork() == 0) { execlp("foot", "foot", (char *)0); }
            break;
        case ACTION_CLOSE:
            if (seat->focused != NULL) {
                river_window_v1_close(seat->focused->obj);
            }

            break;
        case ACTION_FOCUS_NEXT:
            if (!wl_list_empty(&wm.windows)) {
                // Focus the bottom window
                struct Window *window =
                    wl_container_of(wm.windows.next, window, link);
                seat_focus(seat, window);
            }

            break;
        case ACTION_MOVE:
            // Interactive move is only in the floating layout;
            if (wm.layout == LAYOUT_FLOATING && seat->op == SEAT_OP_NONE
                && seat->hovered != NULL) {
                seat_pointer_move(seat, seat->hovered);
            }

            break;
        case ACTION_RESIZE:
            if (wm.layout == LAYOUT_FLOATING && seat->op == SEAT_OP_NONE
                && seat->hovered != NULL) {
                seat_pointer_resize(
                    seat, seat->hovered,
                    RIVER_WINDOW_V1_EDGES_BOTTOM | RIVER_WINDOW_V1_EDGES_RIGHT
                );
            }

            break;
        case ACTION_CYCLE_LAYOUT:
            wm.layout = (wm.layout + 1) % (LAYOUT_LAST + 1);
            break;
        case ACTION_EXIT:
            river_window_manager_v1_exit_session(window_manager_v1);
            break;
    }
}

static void seat_manage(struct Seat *seat) {
    if (seat->new) {
        seat->new = false;

        const uint32_t super = RIVER_SEAT_V1_MODIFIERS_MOD4;
        xkb_binding_create(seat, super, XKB_KEY_Return, ACTION_SPAWN_FOOT);
        xkb_binding_create(seat, super, XKB_KEY_q, ACTION_CLOSE);
        xkb_binding_create(seat, super, XKB_KEY_n, ACTION_FOCUS_NEXT);
        xkb_binding_create(seat, super, XKB_KEY_l, ACTION_CYCLE_LAYOUT);
        xkb_binding_create(seat, super, XKB_KEY_r, ACTION_EXIT);

        pointer_binding_create(seat, super, BTN_LEFT, ACTION_MOVE);
        pointer_binding_create(seat, super, BTN_RIGHT, ACTION_RESIZE);
    }

    // If no window was interacted with in the current manage sequence,
    // intentionally pass NULL to ensure the window on top has focus.
    // This is necessary to handle new windows for example.
    seat_focus(seat, seat->interacted);
    seat->interacted = NULL;

    seat_action(seat, seat->pending_action);
    seat->pending_action = ACTION_NONE;

    switch (seat->op) {
        case SEAT_OP_NONE: break;
        case SEAT_OP_MOVE:
            if (seat->op_release) {
                river_seat_v1_op_end(seat->obj);
                seat->op = SEAT_OP_NONE;
                seat->op_window = NULL;
                break;
            }

            break;
        case SEAT_OP_RESIZE:
            if (seat->op_release) {
                river_window_v1_inform_resize_end(seat->op_window->obj);
                river_seat_v1_op_end(seat->obj);
                seat->op = SEAT_OP_NONE;
                seat->op_window = NULL;
                break;
            }

            int32_t width = seat->op_start_width;
            int32_t height = seat->op_start_height;

            if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_LEFT) != 0) {
                width -= seat->op_dx;
            }

            if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_RIGHT) != 0) {
                width += seat->op_dx;
            }

            if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_TOP) != 0) {
                height -= seat->op_dy;
            }

            if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_BOTTOM) != 0) {
                height += seat->op_dy;
            }

            river_window_v1_propose_dimensions(
                seat->op_window->obj, width > 1 ? width : 1,
                height > 1 ? height : 1
            );

            break;
    }
    seat->op_release = false;
}

static void seat_render(struct Seat *seat) {
    if (wm.layout == LAYOUT_TILING) { return; }

    switch (seat->op) {
        case SEAT_OP_NONE: break;
        case SEAT_OP_MOVE:
            window_set_position(
                seat->op_window, seat->op_start_x + seat->op_dx,
                seat->op_start_y + seat->op_dy
            );

            break;
        case SEAT_OP_RESIZE:;
            int32_t x = seat->op_start_x;
            int32_t y = seat->op_start_y;

            if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_LEFT) != 0) {
                x += seat->op_start_width - seat->op_window->width;
            }

            if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_TOP) != 0) {
                y += seat->op_start_height - seat->op_window->height;
            }

            window_set_position(seat->op_window, x, y);
            break;
    }
}

static void
wm_handle_unavailable(void *data, struct river_window_manager_v1 *obj) {
    fprintf(stderr, "error: another window manager is already running\n");
    exit(1);
}

static void
wm_handle_finished(void *data, struct river_window_manager_v1 *obj) {
    exit(0);
}

static void
wm_handle_manage_start(void *data, struct river_window_manager_v1 *obj) {
    // Destroy closed windows and removed outputs/seats
    struct Output *output, *output_tmp;
    wl_list_for_each_safe(output, output_tmp, &wm.outputs, link) {
        output_maybe_destroy(output);
    }

    struct Window *window, *window_tmp;
    wl_list_for_each_safe(window, window_tmp, &wm.windows, link) {
        window_maybe_destroy(window);
    }

    struct Seat *seat, *seat_tmp;
    wl_list_for_each_safe(seat, seat_tmp, &wm.seats, link) {
        seat_maybe_destroy(seat);
    }

    // Carry out window management policy
    wl_list_for_each(window, &wm.windows, link) { window_manage(window); }
    wl_list_for_each(seat, &wm.seats, link) { seat_manage(seat); }

    // Apply the active layout. Tiled layout overrides
    // any per-window positioning.
    if (wm.layout == LAYOUT_TILING) { layout_tiled_apply(); }

    river_window_manager_v1_manage_finish(window_manager_v1);
}

static void
wm_handle_render_start(void *data, struct river_window_manager_v1 *obj) {
    struct Seat *seat;

    wl_list_for_each(seat, &wm.seats, link) { seat_render(seat); }
    wallpaper_manage(&wallpaper, !wl_list_empty(&wm.windows));
    river_window_manager_v1_render_finish(window_manager_v1);
}

static void wm_handle_window(
    void *data, struct river_window_manager_v1 *obj,
    struct river_window_v1 *river_window
) {
    struct Window *window = calloc(1, sizeof(struct Window));
    window->obj = river_window;
    window->node = river_window_v1_get_node(window->obj);
    window->new = true;

    river_window_v1_add_listener(window->obj, &river_window_listener, window);

    wl_list_insert(wm.windows.prev, &window->link);
}

static void wm_handle_output(
    void *data, struct river_window_manager_v1 *obj,
    struct river_output_v1 *river_output
) {
    struct Output *output = calloc(1, sizeof(struct Output));
    output->obj = river_output;

    river_output_v1_add_listener(output->obj, &river_output_listener, output);

    if (wallpaper.loaded) {
        output->wallpaper = wallpaper_output_create(&wallpaper, river_output);
    }

    wl_list_insert(wm.outputs.prev, &output->link);
}

static void wm_handle_seat(
    void *data, struct river_window_manager_v1 *obj,
    struct river_seat_v1 *river_seat
) {
    struct Seat *seat = calloc(1, sizeof(struct Seat));
    seat->obj = river_seat;
    seat->new = true;

    wl_list_init(&seat->xkb_bindings);
    wl_list_init(&seat->pointer_bindings);

    river_seat_v1_add_listener(seat->obj, &river_seat_listener, seat);

    wl_list_insert(wm.seats.prev, &seat->link);
}

// Ignored events
static void
wm_handle_session_locked(void *data, struct river_window_manager_v1 *obj) {}
static void
wm_handle_session_unlocked(void *data, struct river_window_manager_v1 *obj) {}

static const struct river_window_manager_v1_listener wm_listener = {
    .unavailable = wm_handle_unavailable,
    .finished = wm_handle_finished,
    .manage_start = wm_handle_manage_start,
    .render_start = wm_handle_render_start,
    .session_locked = wm_handle_session_locked,
    .session_unlocked = wm_handle_session_unlocked,
    .window = wm_handle_window,
    .output = wm_handle_output,
    .seat = wm_handle_seat,
};

static void wm_init(void) {
    wl_list_init(&wm.outputs);
    wl_list_init(&wm.windows);
    wl_list_init(&wm.seats);

    // Default layout is tiled (even split).
    wm.layout = LAYOUT_TILING;
}

static void handle_global(
    void *data, struct wl_registry *registry, uint32_t name,
    const char *interface, uint32_t version
) {
    if (strcmp(interface, river_window_manager_v1_interface.name) == 0) {
        if (version >= 4) {
            window_manager_v1 = wl_registry_bind(
                registry, name, &river_window_manager_v1_interface, 4
            );
        }
    } else if (strcmp(interface, river_xkb_bindings_v1_interface.name) == 0) {
        xkb_bindings_v1 = wl_registry_bind(
            registry, name, &river_xkb_bindings_v1_interface, 1
        );
    } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor =
            wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    }
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

int main(void) {
    struct wl_display *display = wl_display_connect(NULL);

    if (display == NULL) {
        fprintf(stderr, "Failed to connect to Wayland server\n");
        return 1;
    }

    // Avoid passing WAYLAND_DEBUG on to our children.
    // It only matters if it's set when the display is created.
    unsetenv("WAYLAND_DEBUG");

    // Ensure children are automatically reaped.
    signal(SIGCHLD, SIG_IGN);

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);

    if (wl_display_roundtrip(display) < 0) {
        fprintf(stderr, "Roundtrip failed\n");
        return 1;
    }

    if (window_manager_v1 == NULL || xkb_bindings_v1 == NULL) {
        fprintf(
            stderr, "river_window_manager_v1 or river_xkb_bindings_v1 "
                    "not supported by the Wayland server\n"
        );

        return 1;
    }

    if (compositor == NULL || shm == NULL) {
        fprintf(
            stderr, "wl_compositor or wl_shm not supported by the Wayland "
                    "server; wallpaper support will be unavailable\n"
        );
    }

    wm_init();

    if (compositor != NULL && shm != NULL) {
        wallpaper_init(&wallpaper, compositor, shm, window_manager_v1);

        const char *wallpaper_path = getenv("NSP_WALLPAPER");
        char default_path[4096];

        if (wallpaper_path == NULL) {
            const char *home = getenv("HOME");

            if (home != NULL) {
                snprintf(
                    default_path, sizeof(default_path),
                    "%s/.config/river/wallpaper.ppm", home
                );
                wallpaper_path = default_path;
            }
        }

        if (wallpaper_path != NULL) {
            if (!wallpaper_load_ppm(&wallpaper, wallpaper_path)) {
                fprintf(stderr, "Wallpaper: continuing without a wallpaper\n");
            }
        }
    }

    river_window_manager_v1_add_listener(window_manager_v1, &wm_listener, NULL);

    while (true) {
        if (wl_display_dispatch(display) < 0) {
            fprintf(stderr, "Dispatch failed\n");
            return 1;
        }
    }

    return 0;
}
