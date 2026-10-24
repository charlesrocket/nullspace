#pragma once

#include "animation.h"
#include "input.h"
#include "layouts/layout.h"
#include "seat.h"
#include "window.h"

#include <nullspace/layouts.h>
#ifdef WALLPAPER
#include "wallpaper.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <wayland-util.h>

#define SPACE_COUNT  10 // TODO aux spaces
#define HIDDEN_POS_X (-1000000)
#define HIDDEN_POS_Y (-1000000)

struct WindowManager {
    struct wl_list windows;        // Window, creation order
    struct wl_list focus_stack;    // Window, most recently focused last
    struct wl_list layer_surfaces; // LayerSurface

    struct AnimationsConfig animations;
#ifdef WALLPAPER
    struct WallpaperConfig wallpaper;
#endif
    struct LibinputConfig libinput;

    enum Layout layout;

    int32_t tiled_gap_outer_h, tiled_gap_outer_v, tiled_gap_inner_h,
        tiled_gap_inner_v;

    int32_t nmasters;

    float mfact;

    int current_space, rounded_corners_radius;

    char *kb_layout;

    bool layout_dirty, rounded_corners, smart_gaps, center_overspread,
        center_when_single_stack, space_urgent[SPACE_COUNT];
};

extern struct WindowManager wm;

void wm_init(void);
void wm_set_layout(enum Layout layout);
void wm_switch_space(int space);
void wm_move_window_to_space(struct Window *window, int space);
void wm_set_kb_layout(const char *layout);
void wm_mark_layout_dirty(void);
void wm_mark_space_urgent(int space);
void wm_assign_new_window_space(struct Window *window, int space);

bool wm_animation_active(void);
void wm_animation_tick_all(void);

struct Window *wm_focus_stack_top(void);
struct Window *wm_next_window(struct Window *window);
struct Window *wm_focus_stack_top_except(struct Window *skip);

#ifdef WALLPAPER
bool wm_set_wallpaper_path(const char *path);
void wm_invalidate_wallpaper(void);
#endif
