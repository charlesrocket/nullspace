#include "seat.h"

#include "input.h"
#include "ipc.h"
#include "manage.h"

#include <river-window-management-v1-client-protocol.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-util.h>

static void
seat_handle_wl_seat(void *data, struct river_seat_v1 *obj, uint32_t id) {}
static void seat_handle_shell_surface_interaction(
    void *data, struct river_seat_v1 *obj,
    struct river_shell_surface_v1 *river_shell_surface
) {}

void seat_maybe_destroy(struct Seat *seat) {
    if (!seat->removed) { return; }

    input_seat_unbind_all(seat);

    if (seat->layer_shell != NULL) {
        river_layer_shell_seat_v1_destroy(seat->layer_shell);
    }

    river_seat_v1_destroy(seat->obj);
    wl_list_remove(&seat->link);
    free(seat);
}

void seat_focus(struct Seat *seat, struct Window *window) {
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

void seat_pointer_move(struct Seat *seat, struct Window *window) {
    seat_focus(seat, window);
    river_seat_v1_op_start_pointer(seat->obj);

    seat->op = SEAT_OP_MOVE;
    seat->op_window = window;
    seat->op_start_x = window->x;
    seat->op_start_y = window->y;
    seat->op_dx = 0;
    seat->op_dy = 0;
}

void seat_pointer_resize(
    struct Seat *seat, struct Window *window, uint32_t edges
) {
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

void seat_action(struct Seat *seat, enum Action action, const void *arg) {
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

void seat_manage(struct Seat *seat) {
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
                int32_t nx = seat->op_start_x + seat->op_dx;
                int32_t ny = seat->op_start_y + seat->op_dy;

                window_set_position(seat->op_window, nx, ny);

                seat->op_window->saved_x = nx;
                seat->op_window->saved_y = ny;
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
                    seat->op_window->saved_x = x;
                    seat->op_window->saved_y = y;
                }

                break;
            }
    }

    seat->op_release = false;
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
