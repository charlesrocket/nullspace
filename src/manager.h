#pragma once

#include "animation.h"
#include "input.h"
#include "layout/layout.h"
#include "output.h"
#include "seat.h"
#include "wallpaper.h"
#include "window.h"

#include <nullspace/layouts.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-util.h>

#define HIDDEN_POS_X (-1000000)
#define HIDDEN_POS_Y (-1000000)

struct WindowManager {
    struct wl_list windows;           // Window, creation order
    struct wl_list focus_stack;       // Window, most recently focused last
    struct wl_list layer_surfaces;    // LayerSurface
    struct wl_list minimized_windows; // Window, newest at .prev

    struct AnimationsConfig animations;
    struct WallpaperConfig wallpaper;
    struct LibinputConfig libinput;

    enum Layout layout;

    uint64_t next_window_id;

    int32_t tiled_gap_outer_h, tiled_gap_outer_v, tiled_gap_inner_h,
        tiled_gap_inner_v, nmasters;

    int rounded_corners_radius;
    char *kb_layout;

    float deco_color_active[4], deco_color_font_active[4],
        deco_color_font_inactive[4], deco_color_inactive[4],
        deco_color_button_active[4], deco_color_button_inactive[4], mfact;

    bool layout_dirty, rounded_corners, resizing, smart_gaps, center_overspread,
        center_when_single_stack;
};

extern struct WindowManager wm;

void wm_init(void);
void wm_finish(void);
void wm_set_layout(enum Layout layout);

void wm_switch_space(int space);
void wm_switch_space_on(struct Output *out, int space);
void wm_move_window_to_space(struct Window *window, int space);
void wm_minimize_window(struct Window *window);
void wm_restore_window(struct Window *window);
void wm_assign_new_window_space(struct Window *window, int space);
void wm_set_kb_layout(const char *layout);
void wm_mark_layout_dirty(void);
void wm_mark_space_urgent(int space);
void wm_mark_space_urgent_on(struct Output *out, int space);

bool wm_animation_active(void);
void wm_animation_tick_all(void);

struct Window *wm_focus_stack_top(void);
struct Window *wm_next_window(struct Window *window);
struct Window *wm_focus_stack_top_except(struct Window *skip);

bool wm_set_wallpaper_path(const char *path);
void wm_invalidate_wallpaper(void);

struct Window *wm_restore_last_minimized(void);
