#ifndef NULLSPACE_H
#define NULLSPACE_H

#include "animation.h"
#include "config.h"
#include "input.h"
#include "layouts/horizontal.h"
#include "layouts/layout.h"
#include "layouts/vertical.h"
#include "output.h"
#include "seat.h"
#ifdef WALLPAPER
#include "wallpaper.h"
#endif

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

#ifdef WALLPAPER
struct WallpaperConfig {
    int32_t topbar_fade_h;

    int32_t pattern_bg_r, pattern_bg_g, pattern_bg_b;
    int32_t pattern_dot_r, pattern_dot_g, pattern_dot_b;
    int32_t pattern_grid_spacing;
    int32_t pattern_dot_radius;

    int32_t blur_radius, blur_passes, blur_top_inset;

    char *path;
};
#endif

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

    int32_t reveal_dir, hide_dir;

    // Size we last proposed to the client.
    int32_t prop_w, prop_h;

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

struct WindowManager {
    struct wl_list outputs;     // Output
    struct wl_list windows;     // Window, creation order (tile order)
    struct wl_list focus_stack; // Window, most recently focused last
    struct wl_list seats;       // Seat
    struct AnimationsConfig animations;
#ifdef WALLPAPER
    struct WallpaperConfig wallpaper;
#endif
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
};

extern struct WindowManager wm;
extern struct river_xkb_bindings_v1 *xkb_bindings_v1;

extern struct river_window_manager_v1 *window_manager_v1;
extern struct river_input_manager_v1 *input_manager_v1;
extern struct river_layer_shell_v1 *layer_shell_v1;
extern struct wl_compositor *compositor;
extern struct wl_shm *shm;

#ifdef WALLPAPER
extern struct Wallpaper wp;
#endif

struct Output *tiling_output(void);

struct Window *focus_stack_top(void);
struct Window *window_next(struct Window *window);

void window_apply_target(
    struct Window *w, int32_t nx, int32_t ny, int32_t nw, int32_t nh
);

void window_set_position(struct Window *window, int32_t x, int32_t y);
void window_send_size(struct Window *window, int32_t w, int32_t h);

void wm_init(void);
void wm_set_layout(enum Layout layout);
void wm_switch_space(int space);
void wm_move_window_to_space(struct Seat *seat, int space);
void wm_request_manage(void);

void wm_handle_unavailable(void *data, struct river_window_manager_v1 *obj);
void wm_handle_finished(void *data, struct river_window_manager_v1 *obj);
void wm_handle_manage_start(void *data, struct river_window_manager_v1 *obj);
void wm_handle_render_start(void *data, struct river_window_manager_v1 *obj);

void wm_handle_seat(
    void *data, struct river_window_manager_v1 *obj,
    struct river_seat_v1 *river_seat
);

// Ignored events
void wm_handle_session_locked(void *data, struct river_window_manager_v1 *obj);

void wm_handle_session_unlocked(
    void *data, struct river_window_manager_v1 *obj
);

void wm_handle_window(
    void *data, struct river_window_manager_v1 *obj,
    struct river_window_v1 *river_window
);

void wm_handle_output(
    void *data, struct river_window_manager_v1 *obj,
    struct river_output_v1 *river_output
);

bool animation_active(void);

#ifdef WALLPAPER
bool wm_set_wallpaper_path(const char *path);
void wm_invalidate_wallpaper(void);
#endif

#endif // NULLSPACE_H
