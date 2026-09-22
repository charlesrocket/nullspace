#include "nullspace.h"

#include "animation.h"
#include "layouts/horizontal.h"
#include "layouts/layout.h"
#include "layouts/trimming.h"
#include "layouts/vertical.h"
#ifdef WALLPAPER
#include "wallpaper.h"

struct Wallpaper wp;
struct wl_shm *shm;
#endif
struct WindowManager wm;

struct river_window_manager_v1 *window_manager_v1;
struct river_xkb_bindings_v1 *xkb_bindings_v1;
struct river_layer_shell_v1 *layer_shell_v1;
struct wl_compositor *compositor;

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
) {
    struct Output *output = data;
    output->pos_x = x;
    output->pos_y = y;
}

static void output_handle_dimensions(
    void *data, struct river_output_v1 *obj, int32_t width, int32_t height
) {
    struct Output *output = data;
    output->width = width;
    output->height = height;

#ifdef WALLPAPER
    if (output->wallpaper != NULL) {
        wallpaper_output_set_dimensions(output->wallpaper, width, height);
    }
#endif
}

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

static void output_maybe_destroy(struct Output *output) {
    if (!output->removed) { return; }
#ifdef WALLPAPER
    if (output->wallpaper != NULL) {
        wallpaper_output_destroy(output->wallpaper);
    }
#endif

    if (output->layer_shell != NULL) {
        river_layer_shell_output_v1_destroy(output->layer_shell);
    }

    river_output_v1_destroy(output->obj);
    wl_list_remove(&output->link);
    free(output);
}

struct Output *tiling_output(void) {
    struct Output *output;
    wl_list_for_each(output, &wm.outputs, link) {
        if (output->removed) { continue; }
        if (output->width <= 0 || output->height <= 0) { continue; }

        return output;
    }

    return NULL;
}

static struct timespec wm_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now;
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

// Start an animation from the current rectangle.
static void window_animate(
    struct Window *window, const struct timespec *now, int32_t target_x,
    int32_t target_y, int32_t target_w, int32_t target_h, int32_t duration
) {
    struct WindowAnimation *a = &window->anim;

    a->start_x = window->x;
    a->start_y = window->y;
    a->start_w = window->prop_w;
    a->start_h = window->prop_h;

    a->target_x = target_x;
    a->target_y = target_y;
    a->target_w = target_w;
    a->target_h = target_h;
    a->duration_ms = duration;
    a->active = true;
    a->start_time = *now;
}

// Start an animation from an explicit rectangle.
static void window_animate_from(
    struct Window *window, const struct timespec *now, int32_t from_x,
    int32_t from_y, int32_t from_w, int32_t from_h, int32_t target_x,
    int32_t target_y, int32_t target_w, int32_t target_h, int32_t duration
) {
    struct WindowAnimation *a = &window->anim;

    a->start_x = from_x;
    a->start_y = from_y;
    a->start_w = from_w;
    a->start_h = from_h;
    a->target_x = target_x;
    a->target_y = target_y;
    a->target_w = target_w;
    a->target_h = target_h;
    a->duration_ms = duration;
    a->active = true;
    a->start_time = *now;
}

static void window_animation_cancel(struct Window *window) {
    window->anim.active = false;
}

static void window_maybe_destroy(struct Window *window) {
    if (!window->closed) { return; }
    if (window->anim.active) { return; } // still shrinking

    if (wm.trimming_tree != NULL && window->in_trimming_tree) {
        trimming_remove(wm.trimming_tree, window);
        window->in_trimming_tree = false;
    }

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
    free(window);
}

// Drive an in-flight animation to its end state.
static void window_animation_finish(struct Window *window) {
    struct WindowAnimation *a = &window->anim;
    if (!a->active) { return; }

    a->active = false;

    window_set_position(window, a->target_x, a->target_y);

    if (!window->closed) {
        window_propose_size(window, a->target_w, a->target_h);
    }
}

// Advances the window's in-flight animation by one tick.
//
// Returns true if the window is still animating afterward, false otherwise.
// MUST check the return value instead of reading `window->anim.active`.
static bool
window_animation_update(struct Window *window, const struct timespec *now) {
    struct WindowAnimation *a = &window->anim;
    if (!a->active) { return false; }

    double t = animation_progress(a, now);
    bool last = t >= 1.0;

    // Do not rely on the easing curve evaluating to even `1.0`.
    double e = last ? 1.0 : ease_out_cubic(t);

    // Round to avoid pixel stalls.
    int32_t x = a->start_x + (int32_t)lround((a->target_x - a->start_x) * e);
    int32_t y = a->start_y + (int32_t)lround((a->target_y - a->start_y) * e);

    window_set_position(window, x, y);

    if (!window->closed) {
        int32_t w =
            a->start_w + (int32_t)lround((a->target_w - a->start_w) * e);
        int32_t h =
            a->start_h + (int32_t)lround((a->target_h - a->start_h) * e);
        window_propose_size(window, w, h);
    }

    if (!last) { return true; }

    a->active = false;
    bool closed = window->closed;

    if (closed) { window_maybe_destroy(window); }
    return false;
}

static void window_handle_closed(void *data, struct river_window_v1 *obj) {
    struct Window *window = data;
    if (window->closed) { return; }
    window->closed = true;

    if (wm.trimming_tree != NULL && window->in_trimming_tree) {
        trimming_remove(wm.trimming_tree, window);
        window->in_trimming_tree = false;
    }

    if (!window->mapped) {
        window_animation_cancel(window);
        return;
    }

    // Shrink to center.
    struct timespec now = wm_now();
    window_animate(
        window, &now, window->x + window->prop_w / 2,
        window->y + window->prop_h / 2, 1, 1, ANIM_DURATION_CLOSE
    );
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

static void seat_pointer_move(struct Seat *seat, struct Window *window);
static void
seat_pointer_resize(struct Seat *seat, struct Window *window, uint32_t edges);

static struct Window *focus_stack_top(void);
static void seat_focus(struct Seat *seat, struct Window *window);

static void window_manage(struct Window *window) {
    if (window->new) {
        window->new = false;
        // Expand the new window.
        window_set_position(window, 0, 0);
        window_propose_size(window, 1, 1);
    }

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

static struct Seat *first_seat_with_pointer(void) {
    struct Seat *seat;
    wl_list_for_each(seat, &wm.seats, link) {
        if (seat->pointer_set) { return seat; }
    }

    return NULL;
}

static struct Window *last_focused_in_tree(void) {
    if (wl_list_empty(&wm.focus_stack)) { return NULL; }

    struct wl_list *cur = wm.focus_stack.prev;
    while (cur != &wm.focus_stack) {
        struct Window *w = wl_container_of(cur, w, focus_link);
        if (w->in_trimming_tree && !w->space_hidden) { return w; }
        cur = cur->prev;
    }

    return NULL;
}

void trimming_sync(int32_t fb_x, int32_t fb_y, int32_t fb_w, int32_t fb_h) {
    if (wm.trimming_tree == NULL) { return; }

    struct Window *window;
    wl_list_for_each(window, &wm.windows, link) {
        if (window->in_trimming_tree) { continue; }
        if (window->closed) { continue; }
        if (window->space_hidden) { continue; }

        struct Window *focused = last_focused_in_tree();
        struct Seat *seat = first_seat_with_pointer();

        int32_t cx, cy;
        if (seat != NULL && seat->pointer_set) {
            cx = seat->pointer_x;
            cy = seat->pointer_y;
        } else {
            cx = fb_x + fb_w;
            cy = fb_y + fb_h;
        }

        int32_t fx = fb_x, fy = fb_y, fw = fb_w, fh = fb_h;
        if (focused != NULL && focused->width > 0 && focused->height > 0) {
            fx = focused->x;
            fy = focused->y;
            fw = focused->width;
            fh = focused->height;
        }

        // Remember the rectangle that is about to be split
        // so the new window can grow out of the correct edge.
        window->spawn_parent_x = fx;
        window->spawn_parent_y = fy;
        window->spawn_parent_w = fw;
        window->spawn_parent_h = fh;
        window->spawn_hint_set = true;

        trimming_insert(
            wm.trimming_tree, window, focused, cx, cy, fx, fy, fw, fh
        );

        window->in_trimming_tree = true;
    }
}

static void spawn_start_rect(
    const struct Window *w, int32_t tx, int32_t ty, int32_t tw, int32_t th,
    int32_t *sx, int32_t *sy, int32_t *sw, int32_t *sh
) {
    // Grow from the center of the target (default/no parent)
    *sx = tx + tw / 2;
    *sy = ty + th / 2;
    *sw = 1;
    *sh = 1;

    if (!w->spawn_hint_set) { return; }

    int32_t px = w->spawn_parent_x, py = w->spawn_parent_y;
    int32_t pw = w->spawn_parent_w, ph = w->spawn_parent_h;

    // Centers of target and parent.
    int32_t tcx = tx + tw / 2, tcy = ty + th / 2;
    int32_t pcx = px + pw / 2, pcy = py + ph / 2;

    int32_t dx = tcx - pcx;
    int32_t dy = tcy - pcy;

    // Given the final target rectangle, and the rectangle of the window it
    // split from, compute a zero-thickness starting rectangle on the shared
    // edge so the new window grows along the split axis only.

    if (abs(dx) >= abs(dy)) {
        // Horizontal split
        // Keep full height, grow in width from shared edge
        *sh = th;
        *sy = ty;
        *sw = 1;
        // New window is to the right with an anchor at its left edge.
        *sx = (dx >= 0) ? tx : tx + tw - 1;
    } else {
        // Vertical split
        // Keep full width, grow in height from shared edge
        *sw = tw;
        *sx = tx;
        *sh = 1;
        *sy = (dy >= 0) ? ty : ty + th - 1;
    }
}

// Drive one window from its current presented rectangle to a target rectangle,
// using the shared animation path.
void window_apply_target(
    struct Window *w, int32_t nx, int32_t ny, int32_t nw, int32_t nh,
    const struct timespec *now
) {
    if (w->space_hidden) { return; }
    if (nw < 1) { nw = 1; }
    if (nh < 1) { nh = 1; }

    if (!w->has_placement) {
        int32_t sx, sy, sw, sh;
        spawn_start_rect(w, nx, ny, nw, nh, &sx, &sy, &sw, &sh);

        window_set_position(w, sx, sy);
        window_propose_size(w, sw, sh);
        w->has_placement = true;

        window_animate_from(
            w, now, sx, sy, sw, sh, nx, ny, nw, nh, ANIM_DURATION_OPEN
        );
    } else if (nx != w->last_target_x || ny != w->last_target_y
               || nw != w->last_target_w || nh != w->last_target_h) {
        int duration = w->space_anim ? ANIM_DURATION_SPACE : ANIM_DURATION_TILE;
        w->space_anim = false;
        window_animate(w, now, nx, ny, nw, nh, duration);
    } else {
        return;
    }

    w->last_target_x = nx;
    w->last_target_y = ny;
    w->last_target_w = nw;
    w->last_target_h = nh;
}

static void wm_set_layout(enum Layout layout) {
    if (wm.layout == layout) { return; }

    struct Window *window;

    // Detach every window from the tree
    if (wm.layout == LAYOUT_TRIMMING && wm.trimming_tree != NULL) {
        wl_list_for_each(window, &wm.windows, link) {
            if (window->in_trimming_tree) {
                trimming_remove(wm.trimming_tree, window);
                window->in_trimming_tree = false;
            }
        }
    }

    wm.layout = layout;

    // Ensure all animations are completed.
    wl_list_for_each(window, &wm.windows, link) {
        // Let the closing windows finish its own animation
        if (window->closed) { continue; }

        // Snap to the end state rather than freezing mid-animation
        // (no infra yet to handle unfinished animations).
        window_animation_finish(window);

        // The client may have changed the size
        // while we were busy with something else.
        window->prop_valid = false;

        // Forces a reset via invalid targets
        // (too hacky?)
        window->last_target_x = INT32_MIN;
        window->last_target_y = INT32_MIN;
        window->last_target_w = INT32_MIN;
        window->last_target_h = INT32_MIN;
    }
}

static void wm_switch_space(int space) {
    if (space < 0 || space >= SPACE_COUNT) { return; }
    if (space == wm.current_space) { return; }

    // +1: left-to-right
    // -1: right-to-left
    int dir = (space > wm.current_space) ? -1 : 1; // animation direction

    struct Output *output = tiling_output();
    int32_t left = HIDDEN_POS_X;
    int32_t right = HIDDEN_POS_X;

    if (output != NULL) {
        int32_t x, w;
        if (output->area_set && output->area_width > 0) {
            x = output->area_x;
            w = output->area_width;
        } else {
            x = output->pos_x;
            w = output->width;
        }

        left = x - w;
        right = x + w;
    }

    struct timespec now = wm_now();
    struct Window *window;

    wl_list_for_each(window, &wm.windows, link) {
        if (window->closed) { continue; }

        if (window->space == space) {
            // Reveal
            if (window->space_hidden) {
                window->space_hidden = false;
                window_animation_cancel(window);

                int32_t start_x = (dir > 0) ? left : right;
                window_set_position(window, start_x, window->y);

                if (wm.layout == LAYOUT_FLOATING) {
                    window_animate_from(
                        window, &now, start_x, window->y, window->prop_w,
                        window->prop_h, window->saved_x, window->saved_y,
                        window->prop_w, window->prop_h, ANIM_DURATION_SPACE
                    );
                } else {
                    window->space_anim = true;
                    window->last_target_x = INT32_MIN;
                    window->last_target_y = INT32_MIN;
                    window->last_target_w = INT32_MIN;
                    window->last_target_h = INT32_MIN;
                }
            }
        } else {
            // Hide
            if (!window->space_hidden) {
                window->space_hidden = true;

                if (window->pos_valid) {
                    window->saved_x = window->x;
                    window->saved_y = window->y;
                } else {
                    window->saved_x = 0;
                    window->saved_y = 0;
                }

                window_animation_cancel(window);

                int32_t target_x = (dir > 0) ? right : left;
                window_animate(
                    window, &now, target_x, window->y, window->prop_w,
                    window->prop_h, ANIM_DURATION_SPACE
                );
            }
        }
    }

    if (wm.trimming_tree != NULL) {
        wl_list_for_each(window, &wm.windows, link) {
            if (window->in_trimming_tree && window->space_hidden) {
                trimming_remove(wm.trimming_tree, window);
                window->in_trimming_tree = false;
            }
        }
    }

    wm.current_space = space;

    // Refocus if needed
    struct Seat *seat;
    wl_list_for_each(seat, &wm.seats, link) {
        if (seat->focused != NULL && !seat->focused->space_hidden) { continue; }
        seat_focus(seat, focus_stack_top());
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
            // Interactive move is only in the floating layout
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
    }
}

static void seat_manage(struct Seat *seat) {
    if (seat->new) {
        seat->new = false;

        const uint32_t super = RIVER_SEAT_V1_MODIFIERS_MOD4;
        xkb_binding_create(seat, super, XKB_KEY_Return, ACTION_SPAWN_FOOT);
        xkb_binding_create(seat, super, XKB_KEY_q, ACTION_CLOSE);
        xkb_binding_create(seat, super, XKB_KEY_f, ACTION_FOCUS_NEXT);
        xkb_binding_create(seat, super, XKB_KEY_l, ACTION_CYCLE_LAYOUT);
        xkb_binding_create(seat, super, XKB_KEY_r, ACTION_EXIT);
        xkb_binding_create(seat, super, XKB_KEY_1, ACTION_SPACE_1);
        xkb_binding_create(seat, super, XKB_KEY_2, ACTION_SPACE_2);
        xkb_binding_create(seat, super, XKB_KEY_3, ACTION_SPACE_3);
        xkb_binding_create(seat, super, XKB_KEY_4, ACTION_SPACE_4);
        xkb_binding_create(seat, super, XKB_KEY_5, ACTION_SPACE_5);
        xkb_binding_create(seat, super, XKB_KEY_6, ACTION_SPACE_6);
        xkb_binding_create(seat, super, XKB_KEY_7, ACTION_SPACE_7);
        xkb_binding_create(seat, super, XKB_KEY_8, ACTION_SPACE_8);
        xkb_binding_create(seat, super, XKB_KEY_9, ACTION_SPACE_9);
        xkb_binding_create(seat, super, XKB_KEY_0, ACTION_SPACE_10);

        pointer_binding_create(seat, super, BTN_LEFT, ACTION_MOVE);
        pointer_binding_create(seat, super, BTN_RIGHT, ACTION_RESIZE);
    }

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

                break;
            }
    }

    seat->op_release = false;
}

static void seat_render(struct Seat *seat) {
    // Tiled layouts place windows deterministically.
    if (layouts_is_tiled(wm.layout)) { return; }

    switch (seat->op) {
        case SEAT_OP_NONE: break;
        case SEAT_OP_MOVE:
            if (seat->op_window->space_hidden) { break; }
            window_set_position(
                seat->op_window, seat->op_start_x + seat->op_dx,
                seat->op_start_y + seat->op_dy
            );

            break;
        case SEAT_OP_RESIZE:
            {
                if (seat->op_window->space_hidden) { break; }

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

    struct timespec now = wm_now(); // keep in sync

    // Carry out window management policy
    wl_list_for_each(window, &wm.windows, link) {
        if (window->closed) { continue; }
        window_manage(window);
    }

    wl_list_for_each(seat, &wm.seats, link) { seat_manage(seat); }

    // Apply the current layout (overrides any per-window positioning).
    layout_apply(&now);

    bool any_animation_active = false;
    struct Window *window_next_safe;
    wl_list_for_each_safe(window, window_next_safe, &wm.windows, link) {
        if (window_animation_update(window, &now)) {
            any_animation_active = true;
        }
    }

    bool any_closed_pending = false;
    wl_list_for_each(window, &wm.windows, link) {
        if (window->closed) {
            any_closed_pending = true;
            break;
        }
    }

    river_window_manager_v1_manage_finish(window_manager_v1);

    if (any_animation_active || any_closed_pending) {
        // Pacing the animations.
        anim_timer_arm(window_manager_v1, &now);
    }
}

static void
wm_handle_render_start(void *data, struct river_window_manager_v1 *obj) {
    struct Seat *seat;

    wl_list_for_each(seat, &wm.seats, link) { seat_render(seat); }
#ifdef WALLPAPER
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

    wallpaper_manage(&wp, has_window, top_zone_h, WALLPAPER_TOPBAR_FADE_H);
#endif
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
    window->space = wm.current_space;

    river_window_v1_add_listener(window->obj, &river_window_listener, window);

    wl_list_insert(wm.windows.prev, &window->link);
    wl_list_insert(wm.focus_stack.prev, &window->focus_link);
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

#ifdef WALLPAPER
    output->wallpaper = wallpaper_output_create(&wp, river_output);

#endif

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

    wm.anim_timer_fd = -1;
    wm.current_space = 0;

    wm.layout = LAYOUT_TRIMMING;
    wm.tiled_gap_outer_h = 8;
    wm.tiled_gap_outer_v = 8;
    wm.tiled_gap_inner_h = 8;
    wm.tiled_gap_inner_v = 8;

    wm.nmasters = 1;
    wm.mfact = 0.55f;
    wm.smart_gaps = false;

    wm.center_overspread = false;
    wm.center_when_single_stack = true;

    wm.trimming_tree = trimming_create();

    if (wm.trimming_tree != NULL) {
        struct TrimmingConfig cfg = {0};

        cfg.manual_split = false;
        cfg.preserve_split = false;
        cfg.smart_split = false;
        cfg.hsplit = 0;
        cfg.vsplit = 0;
        cfg.split_ratio = 0.5f;

        trimming_set_config(wm.trimming_tree, &cfg);
    }

    anim_timer_init();
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
#ifdef WALLPAPER
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
#endif
    }
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static int run_event_loop(struct wl_display *display) {
    while (true) {
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) {
                fprintf(stderr, "Dispatch failed\n");
                return 1;
            }
        }

        // Flush outgoing requests
        short wayland_events = POLLIN;
        if (wl_display_flush(display) < 0) {
            if (errno == EAGAIN) {
                wayland_events |= POLLOUT;
            } else {
                wl_display_cancel_read(display);
                perror("wl_display_flush");
                return 1;
            }
        }

        struct pollfd fds[2] = {
            {.fd = wl_display_get_fd(display), .events = wayland_events},
            // poll() ignores negative fds, so this is safe if timerfd_create
            // failed and anim_timer_fd is -1.
            {          .fd = wm.anim_timer_fd,         .events = POLLIN},
        };

        int ret;
        do { ret = poll(fds, 2, -1); } while (ret < 0 && errno == EINTR);

        if (ret < 0) {
            wl_display_cancel_read(display);
            perror("poll");
            return 1;
        }

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            wl_display_cancel_read(display);
            fprintf(stderr, "Wayland connection lost\n");
            return 1;
        }

        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(display) < 0) {
                perror("wl_display_read_events");
                return 1;
            }
        } else {
            // Nothing to read (e.g. woken by the timer or POLLOUT only).
            wl_display_cancel_read(display);
        }

        if (wl_display_dispatch_pending(display) < 0) {
            fprintf(stderr, "Dispatch failed\n");
            return 1;
        }

        if (wm.anim_timer_fd >= 0 && (fds[1].revents & POLLIN)) {
            anim_timer_fire(window_manager_v1);
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

#ifdef WALLPAPER
    if (shm == NULL) {
        fprintf(
            stderr, "wl_shm not supported by the Wayland server "
                    "(wallpaper support will be unavailable)\n"
        );

        return 1;
    }
#endif

    wm_init();

#ifdef WALLPAPER
    if (compositor != NULL && shm != NULL) {
        wallpaper_init(&wp, compositor, shm, window_manager_v1);

        const char *wallpaper_path = getenv("NSP_WALLPAPER");
        char default_path[4096];

        if (wallpaper_path == NULL) {
            const char *home = getenv("HOME");

            if (home != NULL) {
                snprintf(
                    default_path, sizeof(default_path),
                    "%s/.local/share/nullspace/wallpaper.ppm", home
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
#endif

    river_window_manager_v1_add_listener(window_manager_v1, &wm_listener, NULL);

    return run_event_loop(display);
}
