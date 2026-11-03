#pragma once

#include "config.h"
#include "ipc.h"

#include <ns-util.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-util.h>

struct Window;
struct text_node;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_shadow;

#define DECO_TAB_H CFG_DECO_SLIDER_HEIGHT // slider tab height

struct Decoration {
    struct Window *window;
    struct wlr_scene_tree *tree; // NULL once destroyed
    struct wlr_scene_rect *tab, *btn_close, *btn_zoom, *btn_min;
    struct text_node *title_text; // ?NULL (only when disabled / short tab)

    struct wlr_scene_shadow *shadow_win, *shadow_tab;
    struct wl_listener shadow_win_destroy, shadow_tab_destroy, tree_destroy;

    int32_t tab_x, tab_w, slide_start_tab_x;
    double slide_start_x;
    bool active;
};

void deco_create(struct Window *window);
void deco_destroy(struct Window *window);
void deco_update_position(struct Window *window);
void deco_update_active(struct Window *window, bool active);
void deco_update_for_layout(struct Window *window);
void deco_update_title(struct Window *window);
void deco_refresh_colors(void);

void deco_set_visible(struct Window *window, bool visible);
void deco_raise(struct Window *window);

bool deco_handle_press(double lx, double ly, uint32_t button, uint32_t mods);
bool deco_handle_release(uint32_t button);
bool deco_handle_motion(double lx, double ly);
