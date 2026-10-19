#include "manager.h"

#include "animation.h"
#include "config.h"
#include "ipc.h"
#include "layouts/layout.h"
#include "output.h"
#include "rules.h"
#include "seat.h"
#include "server.h"
#include "window.h"

#include <nullspace/layouts.h>
#ifdef WALLPAPER
#include "wallpaper.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-util.h>

#define LOG_TOPIC "wm"

struct WindowManager wm;

void wm_mark_layout_dirty(void) {
    wm.layout_dirty = true;
    struct Output *o;
    wl_list_for_each(o, &server.outputs, link)
        wlr_output_schedule_frame(o->wlr_output);
}

bool wm_animation_active(void) {
    if (!wm.animations.enabled) return false;
    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->closed) continue;
        if (w->anim.running) return true;
        if (w->hide_dir != 0) return true;
    }

    return false;
}

void wm_animation_tick_all(void) {
    if (!wm.animations.enabled) return;
    int64_t now = animation_now_ms();
    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->closed) continue;
        if (!w->anim.running && w->hide_dir == 0) continue;

        if (w->anim.running) {
            animation_step(&w->anim, now);
            window_set_position(w, w->anim.current.x, w->anim.current.y);

            if (w->anim.running && w->anim.type == ANIM_MOVE) {
                window_set_clip(
                    w, w->anim.current.width, w->anim.current.height
                );
            } else {
                window_clear_clip(w);
            }
        }

        if (w->hide_dir != 0 && !w->anim.running) {
            window_set_position(w, HIDDEN_POS_X, w->y);
            w->hide_dir = 0;
        }
    }
}

struct Window *wm_focus_stack_top(void) {
    if (wl_list_empty(&wm.focus_stack)) return NULL;
    struct wl_list *cur = wm.focus_stack.prev;
    while (cur != &wm.focus_stack) {
        struct Window *w = wl_container_of(cur, w, focus_link);
        if (!w->closed && !w->space_hidden) return w;
        cur = cur->prev;
    }

    return NULL;
}

struct Window *wm_focus_stack_top_except(struct Window *skip) {
    struct Window *w;
    wl_list_for_each_reverse(w, &wm.focus_stack, focus_link) {
        if (w == skip || w->closed || w->space_hidden) continue;
        return w;
    }

    return NULL;
}

struct Window *wm_next_window(struct Window *window) {
    struct wl_list *sentinel = &wm.windows;
    struct wl_list *stop = window ? &window->link : sentinel;
    struct wl_list *cur = stop->next;

    while (cur != stop) {
        if (cur == sentinel) {
            cur = cur->next;
            continue;
        }

        struct Window *w = wl_container_of(cur, w, link);
        if (!w->closed && !w->space_hidden) return w;
        cur = cur->next;
    }

    return NULL;
}

void wm_set_kb_layout(const char *layout) {
    if (layout == NULL) return;

    if (wm.kb_layout != NULL && strcmp(wm.kb_layout, layout) == 0) return;

    free(wm.kb_layout);
    wm.kb_layout = strdup(layout);
    ipc_notify_kb_layout();
}

void wm_set_layout(enum Layout layout) {
    if (wm.layout == layout) return;
    wm.layout = layout;

    // Reparent windows to the correct scene tree.
    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->closed) continue;
        window_reparent(w, !layouts_is_tiled(layout));
        window_set_tiled(w, layouts_is_tiled(layout));
        w->prop_valid = false;
        w->has_target = false;
    }

    ipc_notify_layout();
    wm_mark_layout_dirty();
}

static void window_hide_offscreen(struct Window *window, int32_t dir) {
    if (window->space_hidden) return;

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

    struct Output *out = output_primary();
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

void wm_assign_new_window_space(struct Window *window, int space) {
    if (window == NULL || window->closed) return;
    if (space < 0 || space >= SPACE_COUNT) return;

    window->space = space;

    if (space != wm.current_space) {
        wm_switch_space(space);
        return;
    }

    if (window->space_hidden) {
        window->anim.running = false;
        window->hide_dir = 0;
        window->reveal_dir = 0;
        window->has_target = false;
        window->space_hidden = false;

        window_set_position(window, window->saved_x, window->saved_y);
        wm_mark_layout_dirty();
        idle_update_inhibit();
    }
}

void wm_switch_space(int space) {
    if (space < 0 || space >= SPACE_COUNT) return;
    if (space == wm.current_space) return;

    const int32_t dir = (space > wm.current_space) ? +1 : -1;

    struct Window *window;
    wl_list_for_each(window, &wm.windows, link) {
        if (window->closed) continue;

        if (window->space == space) { // reveal
            if (!window->space_hidden) continue;
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

    struct Window *top = wm_focus_stack_top();
    seat_focus(top);

    ipc_notify_space();
    ipc_notify_focus();
    wm_mark_layout_dirty();
    idle_update_inhibit();
}

void wm_move_window_to_space(struct Window *window, int space) {
    if (space < 0 || space >= SPACE_COUNT) return;
    if (window == NULL || window->closed || window->space_hidden) return;
    if (window->space == space) return;

    window->space = space;

    wm_switch_space(space);
    ipc_notify_window_meta(window);
}

#ifdef WALLPAPER
bool wm_set_wallpaper_path(const char *path) {
    if (path == NULL || path[0] == '\0') return false;

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
    if (new_path == NULL) return false;

    if (!wallpaper_load_image(full)) {
        free(new_path);
        return false;
    }

    free(wm.wallpaper.path);
    wm.wallpaper.path = new_path;
    wallpaper_invalidate_all();
    wm_mark_layout_dirty();
    return true;
}

void wm_invalidate_wallpaper(void) { wallpaper_invalidate_all(); }
#endif

void wm_init(void) {
    wl_list_init(&wm.windows);
    wl_list_init(&wm.focus_stack);
    wl_list_init(&wm.layer_surfaces);

    wm.current_space = 1;
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

    wm.rounded_corners = CFG_ROUNDED_CORNERS_ENABLED;
    wm.rounded_corners_radius = CFG_ROUNDED_CORNERS_RADIUS;

    wm.animations.enabled = CFG_ANIMATIONS_ENABLED;
    wm.animations.open_from_top = CFG_ANIM_OPEN_FROM_TOP;
    wm.animations.duration_move = CFG_ANIM_DURATION_MOVE;
    wm.animations.duration_open = CFG_ANIM_DURATION_OPEN;
    wm.animations.duration_close = CFG_ANIM_DURATION_CLOSE;
    wm.animations.duration_space = CFG_ANIM_DURATION_SPACE;

#ifdef WALLPAPER
    wm.wallpaper.path = strdup(CFG_WALLPAPER_PATH);
    wm.wallpaper.pattern_bg_r = CFG_WALLPAPER_PATTERN_BG_R;
    wm.wallpaper.pattern_bg_g = CFG_WALLPAPER_PATTERN_BG_G;
    wm.wallpaper.pattern_bg_b = CFG_WALLPAPER_PATTERN_BG_B;
    wm.wallpaper.pattern_dot_r = CFG_WALLPAPER_PATTERN_DOT_R;
    wm.wallpaper.pattern_dot_g = CFG_WALLPAPER_PATTERN_DOT_G;
    wm.wallpaper.pattern_dot_b = CFG_WALLPAPER_PATTERN_DOT_B;
    wm.wallpaper.pattern_grid_spacing = CFG_WALLPAPER_PATTERN_GRID_SPACING;
    wm.wallpaper.pattern_dot_radius = CFG_WALLPAPER_PATTERN_DOT_RADIUS;
    wm.wallpaper.blur_enabled = CFG_WALLPAPER_BLUR_ENABLED;
    wm.wallpaper.blur_radius = CFG_BLUR_RADIUS;
    wm.wallpaper.blur_passes = CFG_BLUR_PASSES;
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
    wm.libinput.accel_speed = CFG_LIBINPUT_ACCEL_SPD;
    wm.libinput.click_method = CFG_LIBINPUT_CLICK_METHOD;
    wm.libinput.scroll_method = CFG_LIBINPUT_SCROLL_METHOD;

    wm.kb_layout = NULL;

    animation_init();
    rules_init();
}
