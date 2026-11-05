#pragma once

#include "config.h"
#include "ipc.h"

#include <ns-util.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-util.h>

struct Window;
struct text_node;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_shadow;

#define DECO_TAB_H         CFG_DECO_SLIDER_HEIGHT // slider tab height
#define DECO_BTN_COUNT     2                      // close/minimize
#define DECO_BTN_MAX_PARTS 22

struct DecoButton {
    struct wlr_scene_tree *tree;
    struct wlr_scene_rect *hit;
    struct wlr_scene_rect *parts[DECO_BTN_MAX_PARTS];
    size_t n_parts;
};

struct Decoration {
    struct Window *window;
    struct DecoButton btn[DECO_BTN_COUNT];
    struct wlr_scene_tree *tree; // NULL once destroyed
    struct wlr_scene_rect *tab, *btn_close, *btn_zoom, *btn_min;
    struct wlr_scene_blur *blur_win, *blur_tab;
    struct wlr_scene_buffer *shadow;
    struct wl_listener blur_win_destroy, shadow_destroy, blur_tab_destroy,
        tree_destroy;

    struct text_node *title_text; // ?NULL (only when disabled / short tab)

    uint64_t anim_start_ms;
    uint32_t anim_dur_ms;
    int32_t shadow_key[9], tab_x, tab_w, slide_start_tab_x;

    float progress, // 0 inactive, 1 active
        anim_from, anim_to;

    double slide_start_x;
    bool active, shadow_key_valid, animating;
};

void deco_create(struct Window *window);
void deco_destroy(struct Window *window);
void deco_update_position(struct Window *window);
void deco_update_active(struct Window *window, bool active);
void deco_update_for_layout(struct Window *window);
void deco_update_title(struct Window *window);
void deco_update_blur(struct Window *window);
void deco_refresh_colors(void);

void deco_set_visible(struct Window *window, bool visible);
void deco_raise(struct Window *window);

bool deco_handle_press(double lx, double ly, uint32_t button, uint32_t mods);
bool deco_handle_release(uint32_t button);
bool deco_handle_motion(double lx);
bool deco_anim_init(struct wl_event_loop *loop);
void deco_anim_finish(void);
