#include "decoration.h"

#include "config.h"
#include "cursor.h"
#include "layout/layout.h"
#include "manager.h"
#include "node/text.h"
#include "seat.h"
#include "server.h"
#include "window.h"

#include <dev/evdev/input-event-codes.h>
#include <scenefx/types/fx/clipped_region.h>
#include <scenefx/types/wlr_scene.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>

#define DECO_TAB_H         28
#define DECO_BTN           12
#define DECO_BTN_PAD       6

#define DECO_TAB_MIN_CLOSE (DECO_BTN + 2 * DECO_BTN_PAD)
#define DECO_TAB_MIN_ALL   (3 * DECO_BTN + 4 * DECO_BTN_PAD)
#define DECO_TAB_CHROME    (3 * DECO_BTN + 5 * DECO_BTN_PAD)

#define DECO_TAB_MIN_W     DECO_TAB_MIN_ALL
#define DECO_TAB_TEXT_MAX  900
#define DECO_TAB_MAX_W     (DECO_TAB_TEXT_MAX + DECO_TAB_CHROME)

#define DECO_TITLE_FONT    "Hack Nerd Font 10"

enum DecoPart {
    DECO_PART_TAB,
    DECO_PART_CLOSE,
    DECO_PART_ZOOM,
    DECO_PART_MIN,
};

static void deco_apply_colors(struct Decoration *deco) {
    if (deco == NULL) return;

    const float *tab =
        deco->active ? wm.deco_color_active : wm.deco_color_inactive;
    const float *btn =
        deco->active ? wm.deco_color_button_hl : wm.deco_color_button;

    if (deco->tab != NULL) wlr_scene_rect_set_color(deco->tab, tab);
    if (deco->btn_close != NULL) wlr_scene_rect_set_color(deco->btn_close, btn);
    if (deco->btn_zoom != NULL) wlr_scene_rect_set_color(deco->btn_zoom, btn);
    if (deco->btn_min != NULL) wlr_scene_rect_set_color(deco->btn_min, btn);

    float opacity = CFG_WINDOW_SHADOW_OPACITY;
    if (!deco->active) opacity *= (2.6f / 3.0f);

    float color[4] = {0.0f, 0.0f, 0.0f, opacity};

    if (deco->shadow_win != NULL) {
        wlr_scene_shadow_set_color(deco->shadow_win, color);
    }

    if (deco->shadow_tab != NULL) {
        wlr_scene_shadow_set_color(deco->shadow_tab, color);
    }
}

void deco_refresh_colors(void) {
    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->decoration != NULL) deco_apply_colors(w->decoration);
    }
}

static struct Decoration *sliding_deco = NULL;
static uint32_t swallowed_button = 0;

static void handle_tree_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct Decoration *deco = wl_container_of(l, deco, tree_destroy);

    wl_list_remove(&deco->tree_destroy.link);
    deco->tree = NULL;
    deco->tab = NULL;
    deco->btn_close = NULL;
    deco->btn_zoom = NULL;
    deco->btn_min = NULL;
}

static void handle_shadow_win_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct Decoration *deco = wl_container_of(l, deco, shadow_win_destroy);
    wl_list_remove(&deco->shadow_win_destroy.link);
    deco->shadow_win = NULL;
}

static void handle_shadow_tab_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct Decoration *deco = wl_container_of(l, deco, shadow_tab_destroy);
    wl_list_remove(&deco->shadow_tab_destroy.link);
    deco->shadow_tab = NULL;
}

static void update_tab_corners(struct Decoration *deco, int32_t tab_w) {
    if (deco == NULL || deco->tab == NULL) return;

    int radius = wm.rounded_corners ? wm.rounded_corners_radius : 0;
    if (radius < 0) radius = 0;
    if (radius > tab_w / 2) radius = tab_w / 2;
    if (radius > DECO_TAB_H) radius = DECO_TAB_H;

    uint16_t r = (uint16_t)radius;
    struct fx_corner_radii radii = {
        .top_left = r,
        .top_right = r,
        .bottom_left = 0,
        .bottom_right = 0,
    };

    wlr_scene_rect_set_corner_radii(deco->tab, radii);
}

static int32_t window_w(const struct Window *w) {
    return w->width > 0 ? w->width : 1;
}

static int32_t tab_width_for(const struct Window *w) {
    int32_t ww = window_w(w);

    int32_t natural = 0;
    if (w->decoration != NULL && w->decoration->title_text != NULL) {
        natural = text_node_natural_width(w->decoration->title_text);
    }

    int32_t wanted = natural > 0 ? natural + DECO_TAB_CHROME : DECO_TAB_MIN_W;
    if (wanted < DECO_TAB_MIN_W) wanted = DECO_TAB_MIN_W;
    if (wanted > DECO_TAB_MAX_W) wanted = DECO_TAB_MAX_W;

    return wanted < ww ? wanted : ww;
}

static int32_t tab_x_max(const struct Window *w, int32_t tab_w) {
    int32_t m = window_w(w) - tab_w;
    return m > 0 ? m : 0;
}

static void layout_tab(struct Decoration *deco, int32_t tab_w) {
    if (deco == NULL || deco->tree == NULL || deco->tab == NULL) return;

    deco->tab_w = tab_w;

    const int32_t bx = deco->tab_x;
    const int32_t by = (DECO_TAB_H - DECO_BTN) / 2;

    wlr_scene_node_set_position(&deco->tab->node, bx, 0);
    wlr_scene_rect_set_size(deco->tab, tab_w, DECO_TAB_H);

    update_tab_corners(deco, tab_w);

    wlr_scene_node_set_enabled(
        &deco->btn_close->node, tab_w >= DECO_TAB_MIN_CLOSE
    );

    wlr_scene_node_set_enabled(&deco->btn_min->node, tab_w >= DECO_TAB_MIN_ALL);
    wlr_scene_node_set_enabled(
        &deco->btn_zoom->node, tab_w >= DECO_TAB_MIN_ALL
    );

    wlr_scene_node_set_position(&deco->btn_close->node, bx + DECO_BTN_PAD, by);
    wlr_scene_node_set_position(
        &deco->btn_min->node, bx + 2 * DECO_BTN_PAD + DECO_BTN, by
    );

    wlr_scene_node_set_position(
        &deco->btn_zoom->node, bx + tab_w - DECO_BTN - DECO_BTN_PAD, by
    );

    if (deco->title_text != NULL && deco->title_text->buffer_node != NULL) {
        int32_t avail = tab_w - DECO_TAB_CHROME;
        if (avail < 0) avail = 0;

        text_node_set_max_width(deco->title_text, avail);

        if (avail == 0) {
            wlr_scene_node_set_enabled(
                &deco->title_text->buffer_node->node, false
            );
        } else {
            wlr_scene_node_set_enabled(
                &deco->title_text->buffer_node->node, true
            );

            int th = 0;
            text_node_get_size(deco->title_text, NULL, &th);
            if (th <= 0) th = DECO_BTN;

            int32_t text_x = bx + 3 * DECO_BTN_PAD + 2 * DECO_BTN;
            int32_t text_y = (DECO_TAB_H - th) / 2;
            if (text_y < 0) text_y = 0;

            wlr_scene_node_set_position(
                &deco->title_text->buffer_node->node, text_x, text_y
            );
        }
    }
}

void deco_create(struct Window *window) {
    if (window == NULL || window->decoration != NULL) return;
    if (window->closed || window->fullscreen) return;
    if (layouts_is_tiled(wm.layout)) return;

    struct Decoration *deco = calloc(1, sizeof(*deco));
    if (deco == NULL) return;

    deco->window = window;
    deco->tree = wlr_scene_tree_create(window->scene_tree);
    if (deco->tree == NULL) {
        free(deco);
        return;
    }

    float color[4] = {0.0f, 0.0f, 0.0f, CFG_WINDOW_SHADOW_OPACITY};

    deco->shadow_win = wlr_scene_shadow_create(
        window->scene_tree, 1, 1, CFG_WINDOW_SHADOW_RADIUS,
        CFG_WINDOW_SHADOW_SIGMA, color
    );

    if (deco->shadow_win != NULL) {
        wlr_scene_node_lower_to_bottom(&deco->shadow_win->node);
        deco->shadow_win_destroy.notify = handle_shadow_win_destroy;

        wl_signal_add(
            &deco->shadow_win->node.events.destroy, &deco->shadow_win_destroy
        );
    }

    deco->shadow_tab = wlr_scene_shadow_create(
        window->scene_tree, 1, 1, CFG_WINDOW_SHADOW_RADIUS,
        CFG_WINDOW_SHADOW_SIGMA, color
    );

    if (deco->shadow_tab != NULL) {
        wlr_scene_node_lower_to_bottom(&deco->shadow_tab->node);
        deco->shadow_tab_destroy.notify = handle_shadow_tab_destroy;

        wl_signal_add(
            &deco->shadow_tab->node.events.destroy, &deco->shadow_tab_destroy
        );
    }

    deco->tab = wlr_scene_rect_create(
        deco->tree, DECO_TAB_MAX_W, DECO_TAB_H, wm.deco_color_inactive
    );

    deco->btn_close = wlr_scene_rect_create(
        deco->tree, DECO_BTN, DECO_BTN, wm.deco_color_button
    );

    deco->btn_zoom = wlr_scene_rect_create(
        deco->tree, DECO_BTN, DECO_BTN, wm.deco_color_button
    );

    deco->btn_min = wlr_scene_rect_create(
        deco->tree, DECO_BTN, DECO_BTN, wm.deco_color_button
    );

    if (!deco->tab || !deco->btn_close || !deco->btn_zoom || !deco->btn_min) {
        if (deco->shadow_win) {
            wl_list_remove(&deco->shadow_win_destroy.link);
            wlr_scene_node_destroy(&deco->shadow_win->node);
        }

        if (deco->shadow_tab) {
            wl_list_remove(&deco->shadow_tab_destroy.link);
            wlr_scene_node_destroy(&deco->shadow_tab->node);
        }

        wlr_scene_node_destroy(&deco->tree->node);
        free(deco);
        return;
    }

    deco->tree_destroy.notify = handle_tree_destroy;
    wl_signal_add(&deco->tree->node.events.destroy, &deco->tree_destroy);

    deco->tree->node.data = &window->node_tag;
    wlr_scene_node_set_enabled(&deco->tree->node, window->mapped);

    static const float title_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    deco->title_text =
        text_node_create(deco->tree, DECO_TITLE_FONT, title_color, 1);
    if (deco->title_text != NULL) {
        text_node_set_text(deco->title_text, window->title);
    }

    window->decoration = deco;

    deco_update_position(window);

    deco->active = (window == wm_focus_stack_top());
    deco_apply_colors(deco);
}

void deco_destroy(struct Window *window) {
    if (window == NULL || window->decoration == NULL) return;

    struct Decoration *deco = window->decoration;
    window->decoration = NULL;

    if (sliding_deco == deco) sliding_deco = NULL;

    if (deco->shadow_win != NULL) {
        wl_list_remove(&deco->shadow_win_destroy.link);
        wlr_scene_node_destroy(&deco->shadow_win->node);
        deco->shadow_win = NULL;
    }

    if (deco->shadow_tab != NULL) {
        wl_list_remove(&deco->shadow_tab_destroy.link);
        wlr_scene_node_destroy(&deco->shadow_tab->node);
        deco->shadow_tab = NULL;
    }

    if (deco->tree != NULL) {
        wl_list_remove(&deco->tree_destroy.link);
        wlr_scene_node_destroy(&deco->tree->node);
    }

    if (deco->title_text != NULL) {
        text_node_destroy(deco->title_text);
        deco->title_text = NULL;
    }

    free(deco);
    window_update_corners(window);
}

void deco_update_position(struct Window *window) {
    struct Decoration *deco = window ? window->decoration : NULL;
    if (deco == NULL || deco->tree == NULL || deco->tab == NULL) return;

    int32_t tab_w = tab_width_for(window);
    int32_t max_x = tab_x_max(window, tab_w);
    if (deco->tab_x > max_x) deco->tab_x = max_x;
    if (deco->tab_x < 0) deco->tab_x = 0;

    wlr_scene_node_set_position(&deco->tree->node, 0, -DECO_TAB_H);
    layout_tab(deco, tab_w);

    window_update_corners(window);

    const int sigma = (int)CFG_WINDOW_SHADOW_SIGMA;

    if (deco->shadow_win != NULL) {
        int32_t w = window->width > 0 ? window->width : 1;
        int32_t h = window->height > 0 ? window->height : 1;

        wlr_scene_shadow_set_size(
            deco->shadow_win, w + 2 * sigma, h + 2 * sigma
        );

        wlr_scene_node_set_position(&deco->shadow_win->node, -sigma, -sigma);

        int tl = window->last_tl < 0 ? 0 : window->last_tl;
        int tr = window->last_tr < 0 ? 0 : window->last_tr;
        int bl = window->last_bl < 0 ? 0 : window->last_bl;
        int br = window->last_br < 0 ? 0 : window->last_br;

        struct clipped_region clip = {
            .area = {.x = sigma, .y = sigma, .width = w, .height = h},
            .corners =
                {
                     .top_left = (uint16_t)tl,
                     .top_right = (uint16_t)tr,
                     .bottom_left = (uint16_t)bl,
                     .bottom_right = (uint16_t)br,
                     },
        };

        wlr_scene_shadow_set_clipped_region(deco->shadow_win, clip);
    }

    if (deco->shadow_tab != NULL) {
        wlr_scene_shadow_set_size(
            deco->shadow_tab, tab_w + 2 * sigma, DECO_TAB_H + 2 * sigma
        );

        wlr_scene_node_set_position(
            &deco->shadow_tab->node, deco->tab_x - sigma, -DECO_TAB_H - sigma
        );

        int tab_r = 0;
        if (wm.rounded_corners) {
            tab_r = wm.rounded_corners_radius;
            if (tab_r > tab_w / 2) tab_r = tab_w / 2;
            if (tab_r > DECO_TAB_H) tab_r = DECO_TAB_H;
        }

        struct clipped_region clip = {
            .area =
                {.x = sigma, .y = sigma, .width = tab_w, .height = DECO_TAB_H},
            .corners =
                {
                       .top_left = (uint16_t)tab_r,
                       .top_right = (uint16_t)tab_r,
                       .bottom_left = 0,
                       .bottom_right = 0,
                       },
        };

        wlr_scene_shadow_set_clipped_region(deco->shadow_tab, clip);
    }
}

void deco_update_active(struct Window *window, bool active) {
    struct Decoration *deco = window ? window->decoration : NULL;
    if (deco == NULL || deco->active == active) return;

    deco->active = active;
    deco_apply_colors(deco);
}

void deco_update_for_layout(struct Window *window) {
    if (window == NULL) return;

    if (layouts_is_tiled(wm.layout) || window->fullscreen) {
        deco_destroy(window);
        return;
    }

    if (window->decoration == NULL) deco_create(window);

    struct Decoration *deco = window->decoration;
    if (deco != NULL && deco->tree != NULL) {
        wlr_scene_node_set_enabled(&deco->tree->node, window->mapped);
        deco_update_position(window);
    }
}

void deco_update_title(struct Window *window) {
    if (window == NULL || window->decoration == NULL) return;

    struct Decoration *deco = window->decoration;
    if (deco->title_text == NULL) return;

    text_node_set_text(deco->title_text, window->title);
    deco_update_position(window);
}

void deco_set_visible(struct Window *window, bool visible) {
    struct Decoration *deco = window ? window->decoration : NULL;
    if (deco == NULL || deco->tree == NULL) return;

    if (!visible && sliding_deco == deco) sliding_deco = NULL;
    wlr_scene_node_set_enabled(&deco->tree->node, visible);

    if (deco->shadow_win != NULL) {
        wlr_scene_node_set_enabled(&deco->shadow_win->node, visible);
    }

    if (deco->shadow_tab != NULL) {
        wlr_scene_node_set_enabled(&deco->shadow_tab->node, visible);
    }
}

void deco_raise(struct Window *window) {
    struct Decoration *deco = window ? window->decoration : NULL;
    if (deco == NULL || deco->tree == NULL || window->scene_tree == NULL) {
        return;
    }

    if (deco->tree->node.parent != window->scene_tree->node.parent) return;

    wlr_scene_node_place_above(&deco->tree->node, &window->scene_tree->node);
}

static bool
deco_pick(double lx, double ly, struct Decoration **out, enum DecoPart *part) {
    double nx, ny;
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server.scene->tree.node, lx, ly, &nx, &ny);

    if (node == NULL) return false;

    struct Window *w = window_from_scene_node(node);
    if (w == NULL || w->closed || w->decoration == NULL) return false;

    struct Decoration *deco = w->decoration;

    if (node->type == WLR_SCENE_NODE_RECT) {
        if (deco->tab == NULL) return false;

        if (node == &deco->btn_close->node)
            *part = DECO_PART_CLOSE;
        else if (node == &deco->btn_zoom->node)
            *part = DECO_PART_ZOOM;
        else if (node == &deco->btn_min->node)
            *part = DECO_PART_MIN;
        else if (node == &deco->tab->node)
            *part = DECO_PART_TAB;
        else
            return false;

        *out = deco;
        return true;
    }

    if (node->type == WLR_SCENE_NODE_BUFFER && deco->title_text != NULL
        && deco->title_text->buffer_node != NULL
        && node == &deco->title_text->buffer_node->node) {
        *part = DECO_PART_TAB;
        *out = deco;
        return true;
    }

    return false;
}

static void focus_and_raise(struct Window *w) {
    if (w->closed || !w->mapped || w->space_hidden) return;
    seat_focus(w);
    window_raise(w);
}

bool deco_handle_press(double lx, double ly, uint32_t button, uint32_t mods) {
    swallowed_button = 0;
    sliding_deco = NULL;

    struct Decoration *deco;
    enum DecoPart part;
    if (!deco_pick(lx, ly, &deco, &part)) return false;

    struct Window *w = deco->window;
    swallowed_button = button;

    if (button != BTN_LEFT) return true;

    switch (part) {
        case DECO_PART_CLOSE: window_close(w); return true;

        case DECO_PART_ZOOM:
            focus_and_raise(w);
            window_set_fullscreen(w, !w->fullscreen);
            return true;

        case DECO_PART_MIN: return true;

        case DECO_PART_TAB:
            focus_and_raise(w);
            if (mods & WLR_MODIFIER_SHIFT) {
                sliding_deco = deco;
                deco->slide_start_x = lx;
                deco->slide_start_tab_x = deco->tab_x;
            } else {
                cursor_begin_interactive(w, SEAT_OP_MOVE, 0);
            }

            return true;
    }

    return true;
}

bool deco_handle_release(uint32_t button) {
    if (swallowed_button == 0 || swallowed_button != button) return false;

    swallowed_button = 0;
    sliding_deco = NULL;
    return true;
}

bool deco_handle_motion(double lx, double ly) {
    (void)ly;

    struct Decoration *deco = sliding_deco;
    if (deco == NULL) return false;

    struct Window *w = deco->window;
    if (w == NULL || w->closed || !w->mapped || w->space_hidden
        || w->decoration != deco || deco->tree == NULL) {
        sliding_deco = NULL;
        return false;
    }

    int32_t tab_w = tab_width_for(w);
    double max_x = (double)tab_x_max(w, tab_w);

    double nx = (double)deco->slide_start_tab_x + (lx - deco->slide_start_x);
    if (nx < 0.0) nx = 0.0;
    if (nx > max_x) nx = max_x;

    int32_t new_x = (int32_t)nx;
    if (new_x != deco->tab_x) {
        deco->tab_x = new_x;
        deco_update_position(w);
    }

    return true;
}
