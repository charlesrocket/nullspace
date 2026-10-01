#include "manage.h"

#include "input.h"
#include "ipc.h"
#include "keymap.h"
#include "layouts/horizontal.h"
#include "layouts/layout.h"
#include "layouts/vertical.h"
#ifdef WALLPAPER
#include "wallpaper.h"
#endif

#include <river-libinput-config-v1-client-protocol.h>
#include <sys/event.h>

struct river_window_manager_v1 *window_manager_v1;
struct river_input_manager_v1 *input_manager_v1;
struct river_layer_shell_v1 *layer_shell_v1;
struct wl_compositor *compositor;
struct wl_shm *shm;

#ifdef WALLPAPER
struct Wallpaper wp;
#endif

struct WindowManager wm;

struct river_xkb_bindings_v1 *xkb_bindings_v1;

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

struct Output *tiling_output(void) {
    struct Output *output;
    wl_list_for_each(output, &wm.outputs, link) {
        if (output->removed) { continue; }
        if (output->width <= 0 || output->height <= 0) { continue; }

        return output;
    }

    return NULL;
}

void window_set_position(struct Window *window, int32_t x, int32_t y) {
    if (window->pos_valid && window->x == x && window->y == y) { return; }

    river_node_v1_set_position(window->node, x, y);

    window->x = x;
    window->y = y;
    window->pos_valid = true;
}

void window_send_size(struct Window *window, int32_t w, int32_t h) {
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

bool animation_active(void) {
    if (!wm.animations.enabled) return false;

    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->closed) continue;
        if (w->anim.running) return true;
        if (w->hide_dir != 0) return true;
    }

    return false;
}

static void animation_tick_all(void) {
    if (!wm.animations.enabled) return;

    int64_t now = animation_now_ms();
    struct Window *w;

    wl_list_for_each(w, &wm.windows, link) {
        if (w->closed) continue;
        if (!w->anim.running && w->hide_dir == 0) continue;

        if (w->anim.running) {
            animation_step(&w->anim, now);
            window_set_position(w, w->anim.current.x, w->anim.current.y);

            // A hidden window's size is frozen at the moment of hiding (do not
            // re-propose it while sliding off).
            if (!w->space_hidden) {
                window_propose_size(
                    w, w->anim.current.width, w->anim.current.height
                );
            }
        }

        if (w->hide_dir != 0 && !w->anim.running) {     // finished
            window_set_position(w, HIDDEN_POS_X, w->y); // park
            w->hide_dir = 0;
        }
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

    if (!wm.animations.enabled) {
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
            duration = wm.animations.duration_space;
        } else {
            // Slide from above/below
            from = target;
            from.y = wm.animations.open_from_top ? -target.height
                                                 : target.y + target.height;

            kind = ANIM_OPEN;
            duration = wm.animations.duration_open;
            ;
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
        duration = wm.animations.duration_move;
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

static void window_hide_offscreen(struct Window *window, int32_t dir) {
    if (window->space_hidden) { return; }

    if (window->pos_valid) {
        window->saved_x = window->x;
        window->saved_y = window->y;
    } else {
        window->saved_x = 0;
        window->saved_y = 0;
    }

    window->reveal_dir = 0;
    window->has_target = false;
    window->space_hidden = true;

    if (!wm.animations.enabled || dir == 0) {
        window->anim.running = false;
        window->hide_dir = 0;
        window_set_position(window, HIDDEN_POS_X, window->y);
        return;
    }

    struct Output *out = tiling_output();
    int32_t offset = (out != NULL && out->width > 0) ? out->width : 1920;
    if (offset < 1) offset = 1;

    int32_t nw = (window->prop_valid && window->prop_w > 0)
                   ? window->prop_w
                   : (window->width > 0 ? window->width : 1);
    int32_t nh = (window->prop_valid && window->prop_h > 0)
                   ? window->prop_h
                   : (window->height > 0 ? window->height : 1);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;

    int32_t nx = window->x + dir * offset;
    int32_t ny = window->y;

    struct AnimationBox from =
        window->anim.running
            ? window->anim.current
            : (window->pos_valid && window->width > 0 && window->height > 0
                   ? animation_box(
                         window->x, window->y, window->width, window->height
                     )
                   : animation_box(nx, ny, nw, nh));

    struct AnimationBox target = animation_box(nx, ny, nw, nh);

    window->hide_dir = dir;
    animation_start(
        &window->anim, ANIM_SPACE, &from, &target, wm.animations.duration_space
    );
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

            if (window->hide_dir != 0 && window->anim.running) {
                window->hide_dir = 0;
                window->reveal_dir = 0;
            } else {
                window->hide_dir = 0;
                window->anim.running = false;
                window->has_target = false;
                window->reveal_dir = dir;
                window_set_position(window, window->saved_x, window->saved_y);
            }
        } else { // hide
            window_hide_offscreen(window, -dir);
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

    // The focused window sits on the current space, so the direction of
    // travel is simply target-vs-current.
    const int32_t dir = (space > wm.current_space) ? +1 : -1;

    window->space = space;
    window_hide_offscreen(window, dir);

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

#ifdef WALLPAPER
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
#endif

static void wm_on_kb_layout_changed(const char *layout) {
    if (layout == NULL || layout[0] == '\0') { return; }
    if (wm.kb_layout != NULL && strcmp(wm.kb_layout, layout) == 0) { return; }

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

struct Window *focus_stack_top(void) {
    if (wl_list_empty(&wm.focus_stack)) { return NULL; }

    struct wl_list *cur = wm.focus_stack.prev;
    while (cur != &wm.focus_stack) {
        struct Window *w = wl_container_of(cur, w, focus_link);
        if (!w->closed && !w->space_hidden) { return w; }
        cur = cur->prev;
    }

    return NULL;
}

struct Window *window_next(struct Window *window) {
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

void wm_handle_unavailable(void *data, struct river_window_manager_v1 *obj) {
    fprintf(stderr, "error: another window manager is already running\n");
    exit(1);
}

void wm_handle_finished(void *data, struct river_window_manager_v1 *obj) {
    exit(0);
}

void wm_handle_manage_start(void *data, struct river_window_manager_v1 *obj) {
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

void wm_handle_render_start(void *data, struct river_window_manager_v1 *obj) {
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

#ifdef WALLPAPER
    wallpaper_manage(
        &wp, has_window, top_zone_h, wm.wallpaper.topbar_fade_h,
        wm.wallpaper.topbar_fade_h / 2
    );
#endif

    river_window_manager_v1_render_finish(window_manager_v1);
}

void wm_handle_window(
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

void wm_handle_session_locked(void *data, struct river_window_manager_v1 *obj) {
}

void wm_handle_session_unlocked(
    void *data, struct river_window_manager_v1 *obj
) {}

void wm_handle_output(
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
    output->wallpaper = wallpaper_output_create(&wp);
#endif

    wl_list_insert(wm.outputs.prev, &output->link);
}

void wm_handle_seat(
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

void wm_init(void) {
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

    wm.animations.enabled = CFG_ANIMATIONS;
    wm.animations.open_from_top = CFG_ANIM_OPEN_FROM_TOP;
    wm.animations.duration_move = CFG_ANIM_DURATION_MOVE;
    wm.animations.duration_open = CFG_ANIM_DURATION_OPEN;
    wm.animations.duration_close = CFG_ANIM_DURATION_CLOSE;
    wm.animations.duration_space = CFG_ANIM_DURATION_SPACE;

#ifdef WALLPAPER
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
#endif

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
    animation_init();
}
