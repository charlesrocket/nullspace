#ifndef NULLSPACE_H
#define NULLSPACE_H

#ifdef WALLPAPER
#include "wallpaper.h"
#endif

#include <dev/evdev/input-event-codes.h>
#include <river-layer-shell-v1-client-protocol.h>
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

struct TrimmingTree;

struct Output {
    struct river_output_v1 *obj;
    struct river_layer_shell_output_v1 *layer_shell;
    struct wl_list link; // WindowManager.outputs
#ifdef WALLPAPER
    struct WallpaperOutput *wallpaper;
#endif

    int32_t width;
    int32_t height;
    // Output position in the global coordinate space
    int32_t pos_x;
    int32_t pos_y;
    int32_t area_x;
    int32_t area_y;
    int32_t area_width;
    int32_t area_height;
    bool area_set;
    bool removed;
};

enum Layout {
    LAYOUT_TRIMMING,
    LAYOUT_FLOATING,
    LAYOUT_LAST = LAYOUT_FLOATING,
};

struct Window {
    struct river_window_v1 *obj;
    struct river_node_v1 *node;
    struct wl_list link;       // WindowManager.windows
    struct wl_list focus_link; // WindowManager.focus_stack
    struct Seat *pointer_move_requested;
    struct Seat *pointer_resize_requested;

    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;

    uint32_t pointer_resize_requested_edges;

    enum Layout decoration_state;

    bool decoration_state_set;
    bool in_trimming_tree;

    bool new;
    bool closed;
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

    int32_t op_start_x, op_start_y;
    int32_t op_dx, op_dy;
    // For SEAT_OP_RESIZE only
    int32_t op_start_width, op_start_height;
    uint32_t op_edges;

    int32_t pointer_x;
    int32_t pointer_y;
    bool pointer_set;

    bool op_release;

    bool new;
    bool removed;
};

struct WindowManager {
    struct wl_list outputs;     // Output
    struct wl_list windows;     // Window, creation order (tile order)
    struct wl_list focus_stack; // Window, most recently focused last
    struct wl_list seats;       // Seat
    struct TrimmingTree *trimming_tree;

    enum Layout layout;

    int32_t tiled_gap_outer_h;
    int32_t tiled_gap_outer_v;
    int32_t tiled_gap_inner_h;
    int32_t tiled_gap_inner_v;
};

#endif // NULLSPACE_H
