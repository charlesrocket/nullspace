#ifndef NULLSPACE_H
#define NULLSPACE_H

#include "animation.h"
#include "config.h"
#include "input.h"
#include "layouts/horizontal.h"
#include "layouts/layout.h"
#include "layouts/vertical.h"
#include "output.h"
#include "wallpaper.h"

#include <dev/evdev/input-event-codes.h>
#include <errno.h>
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
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#define SPACE_COUNT  10
#define HIDDEN_POS_X (-1000000)
#define HIDDEN_POS_Y (-1000000)

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

struct WallpaperConfig {
    int32_t topbar_fade_h;

    int32_t pattern_bg_r, pattern_bg_g, pattern_bg_b;
    int32_t pattern_dot_r, pattern_dot_g, pattern_dot_b;
    int32_t pattern_grid_spacing;
    int32_t pattern_dot_radius;

    int32_t blur_radius, blur_passes, blur_top_inset;

    char *path;
};

struct Window {
    struct river_window_v1 *obj;
    struct river_node_v1 *node;
    struct wl_list link;       // WindowManager.windows
    struct wl_list focus_link; // WindowManager.focus_stack
    struct Seat *pointer_move_requested, *pointer_resize_requested;
    struct Animation anim;
    struct AnimationBox target_box;

    enum Layout decoration_state;

    uint32_t pointer_resize_requested_edges;

    int32_t saved_x, saved_y;

    // Position and dimensions last given to the compositor.
    int32_t x, y;
    int32_t width, height;

    // Size we last proposed to the client.
    int32_t prop_w, prop_h;

    int32_t reveal_dir, hide_dir;

    int space;

    char *title;
    char *app_id;

    bool decoration_state_set;
    // The client must report dimensions at least once.
    bool mapped;
    // False until a proposal has been sent/after something invalidates
    // the cache (layout switch). While false, `window_propose_size()` always
    // sends instead of deduplicating.
    bool prop_valid;
    bool pos_valid; // false until a position has been sent

    bool closed;
    // space_hidden == (space != wm.current_space).
    bool space_hidden;
    bool has_target;
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
    struct WallpaperConfig wallpaper;
    struct LibinputConfig libinput;
    struct Output *default_output;

    enum Layout layout;

    int32_t tiled_gap_outer_h;
    int32_t tiled_gap_outer_v;
    int32_t tiled_gap_inner_h;
    int32_t tiled_gap_inner_v;

    int32_t nmasters;
    int current_space;
    float mfact;

    char *kb_layout;

    bool smart_gaps;
    bool center_overspread; // let masters fill width when n <= nmasters
    bool center_when_single_stack;

    bool animations;
};

extern struct WindowManager wm;
extern struct river_xkb_bindings_v1 *xkb_bindings_v1;

struct Output *tiling_output(void);

void window_apply_target(
    struct Window *w, int32_t nx, int32_t ny, int32_t nw, int32_t nh
);

void wm_set_layout(enum Layout layout);
void wm_switch_space(int space);
void wm_move_window_to_space(struct Seat *seat, int space);
void wm_request_manage(void);

bool wm_set_wallpaper_path(const char *path);
void wm_invalidate_wallpaper(void);

#endif // NULLSPACE_H
