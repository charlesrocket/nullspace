#ifndef NULLSPACE_H
#define NULLSPACE_H

#include "config.h"
#include "input.h"
#include "layouts/horizontal.h"
#include "layouts/layout.h"
#include "layouts/vertical.h"
#include "output.h"
#include "wallpaper.h"

#include <dev/evdev/input-event-codes.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <river-input-management-v1-client-protocol.h>
#include <river-layer-shell-v1-client-protocol.h>
#include <river-libinput-config-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>
#include <river-xkb-config-v1-client-protocol.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#define SPACE_COUNT  10
#define HIDDEN_POS_X (-1000000)
#define HIDDEN_POS_Y (-1000000)

struct TrimmingTree;

struct LibinputConfig {
    int tap_state;
    int natural_scroll;
    int left_handed;
    int middle_emulation;
    int dwt;
    int drag;
    int drag_lock;
    int three_finger_drag;
    int accel_profile;
    float accel_speed;
    int click_method;
    int scroll_method;
};

struct AnimConfig {
    int32_t duration_space; // ms
    int32_t duration_open;  // ms, grow
    int32_t duration_close; // ms, shrink
    int32_t duration_tile;  // ms, layout transitions

    int default_hz;
    int min_hz, max_hz;
};

struct WallpaperConfig {
    const char *home_path;
    int32_t topbar_fade_h;
};

struct WindowAnimation {
    struct timespec start_time;

    int32_t duration_ms;

    int32_t start_x, start_y, start_w, start_h;
    int32_t target_x, target_y, target_w, target_h;

    bool active;
};

struct Window {
    struct river_window_v1 *obj;
    struct river_node_v1 *node;
    struct wl_list link;       // WindowManager.windows
    struct wl_list focus_link; // WindowManager.focus_stack
    struct Seat *pointer_move_requested, *pointer_resize_requested;
    struct WindowAnimation anim;

    enum Layout decoration_state;

    uint32_t pointer_resize_requested_edges;

    int32_t saved_x, saved_y;

    // Position and dimensions last given to the compositor.
    int32_t x, y;
    int32_t width, height;

    // Size we last proposed to the client.
    int32_t prop_w, prop_h;

    int32_t last_target_x, last_target_y, last_target_w, last_target_h;
    int32_t spawn_parent_x, spawn_parent_y, spawn_parent_w, spawn_parent_h;

    int space;

    bool spawn_hint_set; // where a new tiled window should appear
    bool decoration_state_set;
    bool in_trimming_tree;
    bool has_placement;
    // The client must report dimensions at least once to receive animations.
    bool mapped;
    // False until a proposal has been sent/after something invalidates
    // the cache (layout switch). While false, `window_propose_size()` always
    // sends instead of deduplicating.
    bool prop_valid;
    bool pos_valid; // false until a position has been sent

    bool new, closed;
    // space_hidden == (space != wm.current_space).
    bool space_hidden, space_anim;
};

struct XkbBinding {
    struct river_xkb_binding_v1 *obj;
    struct Seat *seat;
    struct wl_list link;
    enum Action action;
    const void *arg;
};

struct PointerBinding {
    struct river_pointer_binding_v1 *obj;
    struct Seat *seat;
    struct wl_list link;
    enum Action action;
    const void *arg;
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

struct WindowManager {
    struct wl_list outputs;     // Output
    struct wl_list windows;     // Window, creation order (tile order)
    struct wl_list focus_stack; // Window, most recently focused last
    struct wl_list seats;       // Seat
    struct TrimmingTree *trimming_tree;
    struct AnimConfig anim;
    struct WallpaperConfig wallpaper;
    struct LibinputConfig libinput;

    const char *kb_layout;
    enum Layout layout;

    int64_t anim_frame_ns; // frame interval in nanoseconds

    int32_t tiled_gap_outer_h;
    int32_t tiled_gap_outer_v;
    int32_t tiled_gap_inner_h;
    int32_t tiled_gap_inner_v;

    int32_t nmasters;
    int current_space;
    float mfact;

    bool smart_gaps;
    bool center_overspread; // let masters fill width when n <= nmasters
    bool center_when_single_stack;

    int anim_timer_fd;     // timerfd, or -1 if unavailable
    bool anim_timer_armed; // a tick is already scheduled
};

extern struct WindowManager wm;
extern struct river_xkb_bindings_v1 *xkb_bindings_v1;

struct Output *tiling_output(void);

void trimming_sync(int32_t fb_x, int32_t fb_y, int32_t fb_w, int32_t fb_h);

void window_apply_target(
    struct Window *w, int32_t nx, int32_t ny, int32_t nw, int32_t nh,
    const struct timespec *now
);

#endif // NULLSPACE_H
