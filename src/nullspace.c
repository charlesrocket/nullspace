#include "nullspace.h"

#include "input.h"
#include "ipc.h"
#include "keymap.h"
#include "layouts/horizontal.h"
#include "layouts/layout.h"
#include "layouts/vertical.h"
#include "wallpaper.h"

#include <river-libinput-config-v1-client-protocol.h>
#include <sys/event.h>

struct Wallpaper wp;
struct wl_shm *shm;
struct WindowManager wm;

struct river_window_manager_v1 *window_manager_v1;
struct river_xkb_bindings_v1 *xkb_bindings_v1;
struct river_layer_shell_v1 *layer_shell_v1;
struct river_input_manager_v1 *input_manager_v1;
struct wl_compositor *compositor;

const struct river_output_v1_listener river_output_listener = {
    .removed = output_handle_removed,
    .wl_output = output_handle_wl_output,
    .position = output_handle_position,
    .dimensions = output_handle_dimensions,
};

static void layer_shell_output_handle_non_exclusive_area(
    void *data, struct river_layer_shell_output_v1 *obj, int32_t x, int32_t y,
    int32_t width, int32_t height
) {
    struct Output *output = data;
    output->area_x = x;
    output->area_y = y;
    output->area_width = width;
    output->area_height = height;
    output->area_set = true;
}

static const struct river_layer_shell_output_v1_listener
    river_layer_shell_output_listener = {
        .non_exclusive_area = layer_shell_output_handle_non_exclusive_area,
};

static void
input_manager_handle_finished(void *data, struct river_input_manager_v1 *obj) {}

static void input_manager_handle_input_device(
    void *data, struct river_input_manager_v1 *obj,
    struct river_input_device_v1 *device
) {}

static const struct river_input_manager_v1_listener input_manager_listener = {
    .finished = input_manager_handle_finished,
    .input_device = input_manager_handle_input_device,
};

struct Output *tiling_output(void) {
    struct Output *output;
    wl_list_for_each(output, &wm.outputs, link) {
        if (output->removed) { continue; }
        if (output->width <= 0 || output->height <= 0) { continue; }

        return output;
    }

    return NULL;
}

static void window_set_position(struct Window *window, int32_t x, int32_t y) {
    if (window->pos_valid && window->x == x && window->y == y) { return; }

    river_node_v1_set_position(window->node, x, y);

    window->x = x;
    window->y = y;
    window->pos_valid = true;
}

static void window_send_size(struct Window *window, int32_t w, int32_t h) {
    if (w < 1) { w = 1; }
    if (h < 1) { h = 1; }

    river_window_v1_propose_dimensions(window->obj, w, h);

    window->prop_w = w;
    window->prop_h = h;
    window->prop_valid = true;
}

// Non-interactive cases
static void window_propose_size(struct Window *window, int32_t w, int32_t h) {
    if (w < 1) { w = 1; }
    if (h < 1) { h = 1; }

    // Do not send the same size twice.
    if (window->prop_valid && window->prop_w == w && window->prop_h == h) {
        return;
    }

    window_send_size(window, w, h);
}

static bool animation_active(void) {
    if (!wm.animations) return false;

    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->closed || w->space_hidden) continue;
        if (w->anim.running) return true;
    }

    return false;
}

static void animation_tick_all(void) {
    if (!wm.animations) return;

    int64_t now = animation_now_ms();
    struct Window *w;

    wl_list_for_each(w, &wm.windows, link) {
        if (w->closed || w->space_hidden) continue;
        if (!w->anim.running) continue;

        animation_step(&w->anim, now);
        window_set_position(w, w->anim.current.x, w->anim.current.y);
        window_propose_size(w, w->anim.current.width, w->anim.current.height);
    }
}

static void window_maybe_destroy(struct Window *window) {
    if (!window->closed) { return; }

    struct Seat *seat;

    wl_list_for_each(seat, &wm.seats, link) {
        if (seat->focused == window) { seat->focused = NULL; }
        if (seat->hovered == window) { seat->hovered = NULL; }
        if (seat->interacted == window) { seat->interacted = NULL; }

        if (seat->op_window == window) {
            river_seat_v1_op_end(seat->obj);
            seat->op = SEAT_OP_NONE;
            seat->op_window = NULL;
        }
    }

    river_window_v1_destroy(window->obj);
    wl_list_remove(&window->link);
    wl_list_remove(&window->focus_link);
    ipc_notify_window_closed(window);

    free(window->title);
    free(window->app_id);
    free(window);
}

static void window_handle_closed(void *data, struct river_window_v1 *obj) {
    struct Window *window = data;
    if (window->closed) { return; }
    window->closed = true;
    wm_request_manage();
}

static void window_handle_dimensions(
    void *data, struct river_window_v1 *obj, int32_t width, int32_t height
) {
    struct Window *window = data;
    window->width = width;
    window->height = height;
    window->mapped = true;
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
) {
    struct Window *window = data;
    const char *new_id = app_id ? app_id : "";
    const char *old_id = window->app_id ? window->app_id : "";
    if (strcmp(new_id, old_id) == 0) { return; }

    free(window->app_id);
    window->app_id = app_id ? strdup(app_id) : NULL;
    ipc_notify_window_meta(window);
}

static void window_handle_title(
    void *data, struct river_window_v1 *obj, const char *title
) {
    struct Window *window = data;
    const char *new_title = title ? title : "";
    const char *old_title = window->title ? window->title : "";
    if (strcmp(new_title, old_title) == 0) { return; }

    free(window->title);
    window->title = title ? strdup(title) : NULL;
    ipc_notify_window_meta(window);
}

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

static void seat_pointer_move(struct Seat *seat, struct Window *window);
static void
seat_pointer_resize(struct Seat *seat, struct Window *window, uint32_t edges);

static struct Window *focus_stack_top(void);
static void seat_focus(struct Seat *seat, struct Window *window);

static void window_manage(struct Window *window) {
    if (!window->decoration_state_set
        || window->decoration_state != wm.layout) {

        if (layouts_is_tiled(wm.layout)) {
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

    if (layouts_is_tiled(wm.layout)) {
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

void window_apply_target(
    struct Window *w, int32_t nx, int32_t ny, int32_t nw, int32_t nh
) {
    if (w->space_hidden) { return; }
    if (nw < 1) { nw = 1; }
    if (nh < 1) { nh = 1; }

    // Consume the reveal flag unconditionally
    int32_t reveal_dir = w->reveal_dir;
    w->reveal_dir = 0;

    struct AnimationBox target = animation_box(nx, ny, nw, nh);

    if (w->has_target && animation_box_eq(&w->target_box, &target)) { return; }

    bool first = !w->has_target;
    w->target_box = target;
    w->has_target = true;

    if (!wm.animations) {
        w->anim.kind = ANIM_NONE;
        w->anim.from = target;
        w->anim.to = target;
        w->anim.current = target;
        w->anim.running = false;
        window_set_position(w, nx, ny);
        window_propose_size(w, nw, nh);
        return;
    }

    struct AnimationBox from;
    enum AnimationKind kind;
    int32_t duration;

    if (first && (reveal_dir != 0 || !w->pos_valid)) {
        if (reveal_dir != 0) {
            // Horizontal space slide across the whole output.
            struct Output *out = tiling_output();
            int32_t offset =
                (out != NULL && out->width > 0) ? out->width : target.width;
            if (offset < 1) offset = 1;

            from = target;
            from.x = target.x + reveal_dir * offset;
            kind = ANIM_SPACE;
            duration = CFG_ANIM_DURATION_SPACE;
        } else { // slide from above
            from = target;
            from.y = -target.height;
            kind = ANIM_OPEN;
            duration = CFG_ANIM_DURATION_OPEN;
        }

        window_set_position(w, from.x, from.y);
        window_propose_size(w, target.width, target.height);
    } else {
        if (w->anim.running) {
            from = w->anim.current;
        } else if (w->pos_valid && w->width > 0 && w->height > 0) {
            from = animation_box(w->x, w->y, w->width, w->height);
        } else if (w->pos_valid && w->prop_valid) {
            from = animation_box(w->x, w->y, w->prop_w, w->prop_h);
        } else {
            from = target;
        }

        kind = ANIM_MOVE;
        duration = CFG_ANIM_DURATION_MOVE;
    }

    animation_start(&w->anim, kind, &from, &target, duration);

    window_set_position(w, w->anim.current.x, w->anim.current.y);
    window_propose_size(w, w->anim.current.width, w->anim.current.height);
}

void wm_set_layout(enum Layout layout) {
    if (wm.layout == layout) { return; }

    wm.layout = layout;

    struct Window *window;

    // The client may have changed its size while we were busy.
    wl_list_for_each(window, &wm.windows, link) {
        if (window->closed) { continue; }
        window->prop_valid = false;
    }

    ipc_notify_layout();
}

static void window_hide_offscreen(struct Window *window) {
    if (window->space_hidden) { return; }

    if (window->pos_valid) {
        window->saved_x = window->x;
        window->saved_y = window->y;
    } else {
        window->saved_x = 0;
        window->saved_y = 0;
    }

    window->anim.running = false;

    // Break the `has_target && target_box == target` invariant so the next
    // placement is treated as fresh instead of early-returning.
    window->has_target = false;
    window->reveal_dir = 0;

    window->space_hidden = true;
    window_set_position(window, HIDDEN_POS_X, window->y);
}

void wm_switch_space(int space) {
    if (space < 0 || space >= SPACE_COUNT) { return; }
    if (space == wm.current_space) { return; }

    // Spaces are left-to-right.
    const int32_t dir = (space > wm.current_space) ? +1 : -1;

    struct Window *window;

    wl_list_for_each(window, &wm.windows, link) {
        if (window->closed) { continue; }

        if (window->space == space) { // reveal
            if (!window->space_hidden) { continue; }
            window->space_hidden = false;

            if (wm.layout == LAYOUT_FLOATING) {
                // Floating windows do not go through `layout_apply()`.
                window_set_position(window, window->saved_x, window->saved_y);
                window->has_target = false;
                window->anim.running = false;
                window->reveal_dir = 0;
            } else { // tiling
                // Flag a horizontal space slide for `window_apply_target()`.
                window->reveal_dir = dir;
                window->has_target = false;
                window->anim.running = false;
            }
        } else { // hide
            window_hide_offscreen(window);
        }
    }

    wm.current_space = space;

    struct Seat *seat;
    wl_list_for_each(seat, &wm.seats, link) { // refocus if needed
        if (seat->focused != NULL && !seat->focused->space_hidden) { continue; }
        seat_focus(seat, focus_stack_top());
    }

    ipc_notify_space();
    ipc_notify_focus();
}

void wm_move_window_to_space(struct Seat *seat, int space) {
    if (space < 0 || space >= SPACE_COUNT) { return; }

    struct Window *window = seat->focused;
    if (window == NULL || window->closed || window->space_hidden) { return; }
    if (window->space == space) { return; }

    window->space = space;
    window_hide_offscreen(window);

    struct Seat *s;
    wl_list_for_each(s, &wm.seats, link) {
        if (s->focused != NULL && !s->focused->space_hidden) { continue; }
        seat_focus(s, focus_stack_top());
    }

    ipc_notify_space();
    ipc_notify_window_meta(window);
    ipc_notify_focus();
}

void wm_request_manage(void) {
    if (window_manager_v1 != NULL) {
        river_window_manager_v1_manage_dirty(window_manager_v1);
    }
}

bool wm_set_wallpaper_path(const char *path) {
    if (path == NULL || path[0] == '\0') { return false; }

    char resolved[4096];
    const char *full = path;

    if (path[0] != '/') {
        const char *home = getenv("HOME");
        if (home != NULL) {
            snprintf(resolved, sizeof(resolved), "%s/%s", home, path);
            full = resolved;
        }
    }

    char *new_path = strdup(full);
    if (new_path == NULL) { return false; }

    if (!wallpaper_load_ppm(&wp, full)) {
        free(new_path);
        return false;
    }

    free(wm.wallpaper.path);
    wm.wallpaper.path = new_path;
    wm_invalidate_wallpaper();
    return true;
}

void wm_invalidate_wallpaper(void) {
    wallpaper_invalidate(&wp);
    wm_request_manage();
}

static void wm_on_kb_layout_changed(const char *layout) {
    if (layout == NULL || layout[0] == '\0') { return; }
    if (wm.kb_layout != NULL && strcmp(wm.kb_layout, layout) == 0) {
        return; // unchanged
    }

    char *new_name = strdup(layout);
    if (new_name == NULL) { return; }

    free(wm.kb_layout);
    wm.kb_layout = new_name;

    ipc_notify_kb_layout();
}

static void wm_maybe_set_default_output(void) {
    if (wm.default_output != NULL && !wm.default_output->removed) { return; }
    if (layer_shell_v1 == NULL) { return; }

    struct Output *output;
    wl_list_for_each(output, &wm.outputs, link) {
        if (output->removed) { continue; }
        if (output->layer_shell == NULL) { continue; }

        river_layer_shell_output_v1_set_default(output->layer_shell);
        wm.default_output = output;
        return;
    }
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
) {
    struct Seat *seat = data;
    seat->pointer_x = x;
    seat->pointer_y = y;
    seat->pointer_set = true;
}

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

static void layer_shell_seat_handle_focus_exclusive(
    void *data, struct river_layer_shell_seat_v1 *obj
) {}

static void layer_shell_seat_handle_focus_non_exclusive(
    void *data, struct river_layer_shell_seat_v1 *obj
) {}

static void layer_shell_seat_handle_focus_none(
    void *data, struct river_layer_shell_seat_v1 *obj
) {}

static const struct river_layer_shell_seat_v1_listener
    river_layer_shell_seat_listener = {
        .focus_exclusive = layer_shell_seat_handle_focus_exclusive,
        .focus_non_exclusive = layer_shell_seat_handle_focus_non_exclusive,
        .focus_none = layer_shell_seat_handle_focus_none,
};

static void seat_maybe_destroy(struct Seat *seat) {
    if (!seat->removed) { return; }

    input_seat_unbind_all(seat);

    if (seat->layer_shell != NULL) {
        river_layer_shell_seat_v1_destroy(seat->layer_shell);
    }

    river_seat_v1_destroy(seat->obj);
    wl_list_remove(&seat->link);
    free(seat);
}

static struct Window *focus_stack_top(void) {
    if (wl_list_empty(&wm.focus_stack)) { return NULL; }

    struct wl_list *cur = wm.focus_stack.prev;
    while (cur != &wm.focus_stack) {
        struct Window *w = wl_container_of(cur, w, focus_link);
        if (!w->closed && !w->space_hidden) { return w; }
        cur = cur->prev;
    }

    return NULL;
}

static struct Window *window_next(struct Window *window) {
    if (wl_list_empty(&wm.windows)) { return NULL; }

    struct wl_list *start = (window != NULL) ? &window->link : &wm.windows;
    struct wl_list *cur = start->next;

    while (cur != &wm.windows) {
        struct Window *w = wl_container_of(cur, w, link);
        if (!w->closed && !w->space_hidden) { return w; }
        cur = cur->next;
    }

    return NULL;
}

static void seat_focus(struct Seat *seat, struct Window *window) {
    // Focus the top window (if any) when there is no explicit target.
    if (window == NULL) { window = focus_stack_top(); }
    if (seat->focused == window) { return; }

    if (window != NULL) {
        river_seat_v1_focus_window(seat->obj, window->obj);
        river_node_v1_place_top(window->node);
        wl_list_remove(&window->focus_link);
        wl_list_insert(wm.focus_stack.prev, &window->focus_link);
    } else {
        river_seat_v1_clear_focus(seat->obj);
    }

    seat->focused = window;
    ipc_notify_focus();
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

static void
seat_action(struct Seat *seat, enum Action action, const void *arg) {
    switch (action) {
        case ACTION_NONE: break;
        case ACTION_SPAWN:
            {
                const char *const *argv = arg;
                if (argv == NULL || argv[0] == NULL) { break; }

                if (fork() == 0) {
                    execvp(argv[0], (char *const *)argv);
                    _exit(127);
                }

                break;
            }
        case ACTION_CLOSE:
            if (seat->focused != NULL) {
                struct Window *closing = seat->focused;
                river_window_v1_close(closing->obj);

                // Do not wait for the window destroy, we need to
                // switch the focus right away.
                struct Window *next = NULL;
                struct Window *w;
                wl_list_for_each_reverse(w, &wm.focus_stack, focus_link) {
                    if (w == closing || w->closed) { continue; }
                    if (w->space_hidden) { continue; }
                    next = w;
                    break;
                }

                seat_focus(seat, next);
            }

            break;
        case ACTION_FOCUS_NEXT:
            {
                struct Window *window = window_next(seat->focused);
                if (window != NULL) { seat_focus(seat, window); }

                break;
            }
        case ACTION_MOVE:
            // Interactive move is only in the floating layout.
            if (wm.layout == LAYOUT_FLOATING && seat->op == SEAT_OP_NONE
                && seat->hovered != NULL && !seat->hovered->space_hidden) {
                seat_pointer_move(seat, seat->hovered);
            }

            break;
        case ACTION_RESIZE:
            if (wm.layout == LAYOUT_FLOATING && seat->op == SEAT_OP_NONE
                && seat->hovered != NULL && !seat->hovered->space_hidden) {
                seat_pointer_resize(
                    seat, seat->hovered,
                    RIVER_WINDOW_V1_EDGES_BOTTOM | RIVER_WINDOW_V1_EDGES_RIGHT
                );
            }

            break;
        case ACTION_CYCLE_LAYOUT:
            wm_set_layout((enum Layout)((wm.layout + 1) % (LAYOUT_LAST + 1)));
            break;
        case ACTION_EXIT:
            river_window_manager_v1_exit_session(window_manager_v1);
            break;
        case ACTION_SPACE_1: wm_switch_space(0); break;
        case ACTION_SPACE_2: wm_switch_space(1); break;
        case ACTION_SPACE_3: wm_switch_space(2); break;
        case ACTION_SPACE_4: wm_switch_space(3); break;
        case ACTION_SPACE_5: wm_switch_space(4); break;
        case ACTION_SPACE_6: wm_switch_space(5); break;
        case ACTION_SPACE_7: wm_switch_space(6); break;
        case ACTION_SPACE_8: wm_switch_space(7); break;
        case ACTION_SPACE_9: wm_switch_space(8); break;
        case ACTION_SPACE_10: wm_switch_space(9); break;
        case ACTION_MOVE_TO_SPACE_1: wm_move_window_to_space(seat, 0); break;
        case ACTION_MOVE_TO_SPACE_2: wm_move_window_to_space(seat, 1); break;
        case ACTION_MOVE_TO_SPACE_3: wm_move_window_to_space(seat, 2); break;
        case ACTION_MOVE_TO_SPACE_4: wm_move_window_to_space(seat, 3); break;
        case ACTION_MOVE_TO_SPACE_5: wm_move_window_to_space(seat, 4); break;
        case ACTION_MOVE_TO_SPACE_6: wm_move_window_to_space(seat, 5); break;
        case ACTION_MOVE_TO_SPACE_7: wm_move_window_to_space(seat, 6); break;
        case ACTION_MOVE_TO_SPACE_8: wm_move_window_to_space(seat, 7); break;
        case ACTION_MOVE_TO_SPACE_9: wm_move_window_to_space(seat, 8); break;
        case ACTION_MOVE_TO_SPACE_10: wm_move_window_to_space(seat, 9); break;
    }
}

static void seat_manage(struct Seat *seat) {
    if (seat->new) {
        seat->new = false;
        input_seat_bind_all(seat);
    }

    seat_focus(seat, seat->interacted);
    seat->interacted = NULL;

    seat_action(seat, seat->pending_action, seat->pending_arg);
    seat->pending_action = ACTION_NONE;
    seat->pending_arg = NULL;

    switch (seat->op) {
        case SEAT_OP_NONE: break;
        case SEAT_OP_MOVE:
            if (seat->op_release) {
                river_seat_v1_op_end(seat->obj);
                seat->op = SEAT_OP_NONE;
                seat->op_window = NULL;
                break;
            }

            if (seat->op_window != NULL && !seat->op_window->space_hidden) {
                window_set_position(
                    seat->op_window, seat->op_start_x + seat->op_dx,
                    seat->op_start_y + seat->op_dy
                );
            }

            break;
        case SEAT_OP_RESIZE:
            {
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

                window_send_size(seat->op_window, width, height);

                if (seat->op_window != NULL && !seat->op_window->space_hidden) {
                    int32_t x = seat->op_start_x;
                    int32_t y = seat->op_start_y;

                    if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_LEFT) != 0) {
                        x += seat->op_start_width - seat->op_window->width;
                    }

                    if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_TOP) != 0) {
                        y += seat->op_start_height - seat->op_window->height;
                    }

                    window_set_position(seat->op_window, x, y);
                }

                break;
            }
    }

    seat->op_release = false;
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
    struct Output *output, *output_tmp;
    wl_list_for_each_safe(output, output_tmp, &wm.outputs, link) {
        // Destroy closed windows and removed outputs/seats.
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

    wm_maybe_set_default_output();
    ipc_process_pending_commands();

    // Carry out window management policy.
    wl_list_for_each(window, &wm.windows, link) {
        if (window->closed) { continue; }
        window_manage(window);
    }

    wl_list_for_each(seat, &wm.seats, link) { seat_manage(seat); }

    layout_apply();
    animation_tick_all(); // by river protocol

    // Sweep reveal flags (`layout_apply()` leftovers).
    wl_list_for_each(window, &wm.windows, link) {
        if (window->reveal_dir == 0) continue;
        if (!window->space_hidden) {
            window->anim.running = false;
            window->has_target = false;

            window_set_position(window, window->saved_x, window->saved_y);
        }

        window->reveal_dir = 0;
    }

    if (animation_active()) { wm_request_manage(); }

    river_window_manager_v1_manage_finish(window_manager_v1);
}

static void
wm_handle_render_start(void *data, struct river_window_manager_v1 *obj) {
    bool has_window = false;
    struct Window *window;
    wl_list_for_each(window, &wm.windows, link) {
        if (!window->closed && !window->space_hidden) {
            has_window = true;
            break;
        }
    }

    int32_t top_zone_h = 0;
    if (!has_window) {
        struct Output *output;
        wl_list_for_each(output, &wm.outputs, link) {
            if (!output->area_set) { continue; }
            if (output->width <= 0 || output->height <= 0) { continue; }
            if (output->area_width <= 0 || output->area_height <= 0) {
                continue;
            }

            int32_t strip = output->area_y - output->pos_y;

            if (strip < 0) { strip = 0; }
            if (strip > output->height) { strip = output->height; }
            if (strip > top_zone_h) { top_zone_h = strip; }
        }
    }

    wallpaper_manage(
        &wp, has_window, top_zone_h, wm.wallpaper.topbar_fade_h,
        wm.wallpaper.topbar_fade_h / 2
    );

    river_window_manager_v1_render_finish(window_manager_v1);
}

static void wm_handle_window(
    void *data, struct river_window_manager_v1 *obj,
    struct river_window_v1 *river_window
) {
    struct Window *window = calloc(1, sizeof(struct Window));
    window->obj = river_window;
    window->node = river_window_v1_get_node(window->obj);
    window->space = wm.current_space;

    // Animation state is zero-initialized for default negatives.

    river_window_v1_add_listener(window->obj, &river_window_listener, window);

    wl_list_insert(wm.windows.prev, &window->link);
    wl_list_insert(wm.focus_stack.prev, &window->focus_link);
    ipc_notify_window_opened(window);
}

static void wm_handle_output(
    void *data, struct river_window_manager_v1 *obj,
    struct river_output_v1 *river_output
) {
    struct Output *output = calloc(1, sizeof(struct Output));
    output->obj = river_output;

    river_output_v1_add_listener(output->obj, &river_output_listener, output);

    if (layer_shell_v1 != NULL) {
        output->layer_shell =
            river_layer_shell_v1_get_output(layer_shell_v1, river_output);

        river_layer_shell_output_v1_add_listener(
            output->layer_shell, &river_layer_shell_output_listener, output
        );
    }

    output->wallpaper = wallpaper_output_create(&wp);

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

    if (layer_shell_v1 != NULL) {
        seat->layer_shell =
            river_layer_shell_v1_get_seat(layer_shell_v1, river_seat);

        river_layer_shell_seat_v1_add_listener(
            seat->layer_shell, &river_layer_shell_seat_listener, seat
        );
    }

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
    wl_list_init(&wm.focus_stack);
    wl_list_init(&wm.seats);

    wm.current_space = 0;
    wm.default_output = NULL;

    wm.layout = CFG_DEFAULT_LAYOUT;

    wm.tiled_gap_outer_h = CFG_GAP_OUTER_H;
    wm.tiled_gap_outer_v = CFG_GAP_OUTER_V;
    wm.tiled_gap_inner_h = CFG_GAP_INNER_H;
    wm.tiled_gap_inner_v = CFG_GAP_INNER_V;
    wm.smart_gaps = CFG_SMART_GAPS;

    wm.nmasters = CFG_NMASTERS;
    wm.mfact = CFG_MFACT;
    wm.center_overspread = CFG_CENTER_OVERSPREAD;
    wm.center_when_single_stack = CFG_CENTER_WHEN_SINGLE_STACK;

    wm.animations = CFG_ANIMATIONS;
    animation_init();

    wm.wallpaper.path = strdup(CFG_WALLPAPER_PATH);
    wm.wallpaper.topbar_fade_h = CFG_WALLPAPER_TOPBAR_FADE_H;

    wm.wallpaper.pattern_bg_r = CFG_WALLPAPER_PATTERN_BG_R;
    wm.wallpaper.pattern_bg_g = CFG_WALLPAPER_PATTERN_BG_G;
    wm.wallpaper.pattern_bg_b = CFG_WALLPAPER_PATTERN_BG_B;
    wm.wallpaper.pattern_dot_r = CFG_WALLPAPER_PATTERN_DOT_R;
    wm.wallpaper.pattern_dot_g = CFG_WALLPAPER_PATTERN_DOT_G;
    wm.wallpaper.pattern_dot_b = CFG_WALLPAPER_PATTERN_DOT_B;
    wm.wallpaper.pattern_grid_spacing = CFG_WALLPAPER_PATTERN_GRID_SPACING;
    wm.wallpaper.pattern_dot_radius = CFG_WALLPAPER_PATTERN_DOT_RADIUS;
    wm.wallpaper.blur_radius = CFG_WALLPAPER_BLUR_RADIUS;
    wm.wallpaper.blur_passes = CFG_WALLPAPER_BLUR_PASSES;
    wm.wallpaper.blur_top_inset = CFG_WALLPAPER_BLUR_TOP_INSET;

    wm.libinput.tap_state = CFG_LIBINPUT_TAP_STATE;
    wm.libinput.natural_scroll = CFG_LIBINPUT_NATURAL_SCROLL;
    wm.libinput.left_handed = CFG_LIBINPUT_LEFT_HANDED;
    wm.libinput.middle_emulation = CFG_LIBINPUT_MIDDLE_EMULATION;
    wm.libinput.dwt = CFG_LIBINPUT_DWT;
    wm.libinput.drag = CFG_LIBINPUT_DRAG;
    wm.libinput.drag_lock = CFG_LIBINPUT_DRAG_LOCK;
    wm.libinput.three_finger_drag = CFG_LIBINPUT_THREE_FINGER_DRAG;
    wm.libinput.accel_profile = CFG_LIBINPUT_ACCEL_PROFILE;
    wm.libinput.accel_speed = CFG_LIBINPUT_ACCEL_SPEED;
    wm.libinput.click_method = CFG_LIBINPUT_CLICK_METHOD;
    wm.libinput.scroll_method = CFG_LIBINPUT_SCROLL_METHOD;

    wm.kb_layout = NULL;

    keymap_set_layout_callback(wm_on_kb_layout_changed);
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
    } else if (strcmp(interface, river_layer_shell_v1_interface.name) == 0) {
        layer_shell_v1 = wl_registry_bind(
            registry, name, &river_layer_shell_v1_interface, 1
        );
    } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor =
            wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, river_xkb_config_v1_interface.name) == 0) {
        keymap_bind(registry, name);
    } else if (strcmp(interface, river_libinput_config_v1_interface.name)
               == 0) {
        libinput_bind(registry, name);
    } else if (strcmp(interface, river_input_manager_v1_interface.name) == 0) {
        input_manager_v1 = wl_registry_bind(
            registry, name, &river_input_manager_v1_interface, 2
        );

        river_input_manager_v1_add_listener(
            input_manager_v1, &input_manager_listener, NULL
        );
    }
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static int run_event_loop(struct wl_display *display) {
    int wl_fd = wl_display_get_fd(display);

    int kq = kqueue();
    if (kq < 0) {
        perror("kqueue");
        return 1;
    }

    struct kevent ev;

    EV_SET(&ev, (uintptr_t)wl_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
        perror("kevent(wayland)");
        close(kq);
        return 1;
    }

    ipc_kqueue_register(kq);

    bool wl_write_armed = false;

    struct kevent events[256];

    while (true) {
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) {
                fprintf(stderr, "Dispatch failed\n");
                close(kq);
                return 1;
            }
        }

        if (wl_display_flush(display) < 0) {
            if (errno == EAGAIN) {
                if (!wl_write_armed) {
                    EV_SET(
                        &ev, (uintptr_t)wl_fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL
                    );
                    if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
                        wl_display_cancel_read(display);
                        perror("kevent(wayland write)");
                        close(kq);
                        return 1;
                    }

                    wl_write_armed = true;
                }
            } else {
                wl_display_cancel_read(display);
                perror("wl_display_flush");
                close(kq);
                return 1;
            }
        } else if (wl_write_armed) {
            EV_SET(&ev, (uintptr_t)wl_fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
            if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0 && errno != ENOENT) {
                wl_display_cancel_read(display);
                perror("kevent(wayland write delete)");
                close(kq);
                return 1;
            }

            wl_write_armed = false;
        }

        ipc_flush_pending();

        struct timespec timeout = {.tv_sec = 0, .tv_nsec = 16 * 1000 * 1000};
        struct timespec *timeoutp = animation_active() ? &timeout : NULL;

        int n = kevent(kq, NULL, 0, events, 256, timeoutp);
        if (n < 0) {
            if (errno == EINTR) {
                wl_display_cancel_read(display);
                continue;
            }

            wl_display_cancel_read(display);
            perror("kevent");
            close(kq);
            return 1;
        }

        // Timeout fired with no events: ask for another manage pass so the
        // animation keeps stepping.
        if (n == 0 && animation_active()) { wm_request_manage(); }

        bool wl_readable = false;

        for (int i = 0; i < n; i++) {
            const struct kevent *e = &events[i];
            int fd = (int)e->ident;

            if (fd == wl_fd) {
                if (e->filter == EVFILT_READ) {
                    wl_readable = true;
                } else if (e->filter == EVFILT_WRITE) {
                    if (wl_display_flush(display) == 0) {
                        EV_SET(
                            &ev, (uintptr_t)wl_fd, EVFILT_WRITE, EV_DELETE, 0,
                            0, NULL
                        );

                        if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0
                            && errno != ENOENT) {
                            wl_display_cancel_read(display);
                            perror("kevent(wayland write delete)");
                            close(kq);
                            return 1;
                        }
                        wl_write_armed = false;
                    } else if (errno != EAGAIN) {
                        wl_display_cancel_read(display);
                        perror("wl_display_flush");
                        close(kq);
                        return 1;
                    }
                }

                continue;
            }

            ipc_kqueue_handle(e);
        }

        if (wl_readable) {
            if (wl_display_read_events(display) < 0) {
                perror("wl_display_read_events");
                close(kq);
                return 1;
            }
        } else {
            // Nothing to read (e.g. woken by POLLOUT only).
            wl_display_cancel_read(display);
        }

        if (wl_display_dispatch_pending(display) < 0) {
            fprintf(stderr, "Dispatch failed\n");
            close(kq);
            return 1;
        }
    }
}

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

    wm_init();

    ipc_init();
    keymap_init();
    libinput_init();

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

    if (layer_shell_v1 == NULL) {
        fprintf(
            stderr, "river_layer_shell_v1 not supported by the Wayland "
                    "server (layer surfaces will be unavailable)\n"
        );
    }

    if (compositor == NULL) {
        fprintf(stderr, "wl_compositor not supported by the Wayland server\n");
        return 1;
    }

    if (shm == NULL) {
        fprintf(
            stderr, "wl_shm not supported by the Wayland server "
                    "(wallpaper support will be unavailable)\n"
        );
    }

    wallpaper_init(&wp, compositor, shm, window_manager_v1);

    if (compositor != NULL && shm != NULL) {
        const char *wallpaper_path = getenv("NSP_WALLPAPER");
        char default_path[4096];

        if (wallpaper_path == NULL && wm.wallpaper.path != NULL) {
            const char *home = getenv("HOME");

            if (home != NULL) {
                snprintf(
                    default_path, sizeof(default_path), "%s/%s", home,
                    wm.wallpaper.path
                );

                wallpaper_path = default_path;
            }
        }

        if (wallpaper_path != NULL) {
            if (!wallpaper_load_ppm(&wp, wallpaper_path)) {
                fprintf(stderr, "Wallpaper: using default pattern\n");
            }
        }
    }

    river_window_manager_v1_add_listener(window_manager_v1, &wm_listener, NULL);

    int rc = run_event_loop(display);
    ipc_destroy();
    return rc;
}
