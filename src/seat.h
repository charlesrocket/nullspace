#ifndef SEAT_H
#define SEAT_H

#include "actions.h"

#include <stdbool.h>
#include <stdint.h>
#include <wayland-util.h>

struct Window;
struct river_seat_v1;
struct river_layer_shell_seat_v1;
struct river_seat_v1_listener;

enum SeatOp {
    SEAT_OP_NONE,
    SEAT_OP_MOVE,
    SEAT_OP_RESIZE,
};

extern const struct river_seat_v1_listener river_seat_listener;

struct Seat {
    struct river_seat_v1 *obj;
    struct river_layer_shell_seat_v1 *layer_shell;
    struct wl_list link; // WindowManager.seats

    struct Window *focused;
    struct Window *hovered;
    struct Window *interacted;
    // For SEAT_OP_MOVE and SEAT_OP_RESIZE
    struct Window *op_window;

    struct wl_list xkb_bindings;     // XkbBinding
    struct wl_list pointer_bindings; // PointerBinding

    enum Action pending_action;
    enum SeatOp op;

    const void *pending_arg;

    uint32_t op_edges;

    int32_t op_start_x, op_start_y;
    int32_t op_dx, op_dy;
    // For SEAT_OP_RESIZE only
    int32_t op_start_width;
    int32_t op_start_height;

    int32_t pointer_x;
    int32_t pointer_y;

    bool pointer_set;
    bool op_release;

    bool new;
    bool removed;
};

void seat_maybe_destroy(struct Seat *seat);
void seat_focus(struct Seat *seat, struct Window *window);
void seat_pointer_move(struct Seat *seat, struct Window *window);
void seat_pointer_resize(
    struct Seat *seat, struct Window *window, uint32_t edges
);

void seat_action(struct Seat *seat, enum Action action, const void *arg);
void seat_manage(struct Seat *seat);

struct Window *focus_stack_top(void);

#endif // SEAT_H
