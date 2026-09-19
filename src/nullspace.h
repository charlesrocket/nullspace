#ifndef NULLSPACE_H
#define NULLSPACE_H

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

struct Output {
    struct river_output_v1 *obj;
    struct wl_list link; // WindowManager.outputs
    struct WallpaperOutput *wallpaper;

    int32_t width;
    int32_t height;
    bool removed;
};

enum Layout {
    LAYOUT_TILING,
    LAYOUT_FLOATING,
    LAYOUT_LAST = LAYOUT_FLOATING,
};

#define TILED_GAP 8

struct Window {
    struct river_window_v1 *obj;
    struct river_node_v1 *node;

    bool new;
    bool closed;

    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;

    struct Seat *pointer_move_requested;
    struct Seat *pointer_resize_requested;
    uint32_t pointer_resize_requested_edges;

    enum Layout decoration_state;
    bool decoration_state_set;

    struct wl_list link; // WindowManager.windows
};

enum Action {
    ACTION_NONE,
    ACTION_SPAWN_FOOT,
    ACTION_CLOSE,
    ACTION_FOCUS_NEXT,
    ACTION_MOVE,
    ACTION_RESIZE,
    ACTION_CYCLE_LAYOUT,
    ACTION_EXIT,
};

struct XkbBinding {
    struct river_xkb_binding_v1 *obj;
    struct Seat *seat;
    enum Action action;
    struct wl_list link;
};

struct PointerBinding {
    struct river_pointer_binding_v1 *obj;
    struct Seat *seat;
    enum Action action;
    struct wl_list link;
};

enum SeatOp {
    SEAT_OP_NONE,
    SEAT_OP_MOVE,
    SEAT_OP_RESIZE,
};

struct Seat {
    struct river_seat_v1 *obj;
    bool new;
    bool removed;

    struct Window *focused;
    struct Window *hovered;
    struct Window *interacted;
    // For SEAT_OP_MOVE and SEAT_OP_RESIZE
    struct Window *op_window;

    struct wl_list xkb_bindings;     // XkbBinding
    struct wl_list pointer_bindings; // PointerBinding
    enum Action pending_action;

    enum SeatOp op;

    int32_t op_start_x, op_start_y;
    int32_t op_dx, op_dy;
    bool op_release;
    // For SEAT_OP_RESIZE only
    int32_t op_start_width, op_start_height;
    uint32_t op_edges;

    struct wl_list link; // WindowManager.seats
};

struct WindowManager {
    struct wl_list outputs; // Output
    struct wl_list windows; // Window
    struct wl_list seats;   // Seat

    enum Layout layout;
};

struct WindowManager wm;
struct Wallpaper wallpaper;

struct river_window_manager_v1 *window_manager_v1;
struct river_xkb_bindings_v1 *xkb_bindings_v1;
struct wl_compositor *compositor;
struct wl_shm *shm;

#endif // NULLSPACE_H
