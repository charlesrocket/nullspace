#include "layout.h"

#include "../config.h"
#include "../manager.h"
#include "../output.h"
#include "../server.h"
#include "../window.h"
#include "horizontal.h"
#include "vertical.h"

#include <nullspace/layouts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <wayland-util.h>

static int32_t cascade_next_slot = 0;

static void output_usable_area(
    struct Output* output, int32_t* x, int32_t* y, int32_t* w, int32_t* h
) {
    if (!output->area_set || output->area_width <= 0
        || output->area_height <= 0) {
        *x = output->pos_x;
        *y = output->pos_y;
        *w = output->width;
        *h = output->height;

        return;
    }

    *x = output->area_x;
    *y = output->area_y;
    *w = output->area_width;
    *h = output->area_height;
}

static bool window_is_layoutable(const struct Window* w) {
    return !w->closed && !w->space_hidden && w->mapped && !w->fullscreen
        && !window_is_transient(w);
}

static struct Window* layout_focused_window_for(struct Output* out) {
    struct Window* window;
    wl_list_for_each_reverse(window, &wm.focus_stack, focus_link) {
        if (window->output != out) continue;
        if (window_is_layoutable(window)) return window;
    }

    return NULL;
}

static void layout_output(struct Output* output, LayoutCompute compute) {
    if (output->removed) return;
    if (output->wlr_output == NULL || !output->wlr_output->enabled) return;

    int32_t ax, ay, aw, ah;
    output_usable_area(output, &ax, &ay, &aw, &ah);
    if (aw <= 0 || ah <= 0) return;

    size_t n = 0;
    struct Window* w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->output != output) continue;
        if (window_is_layoutable(w)) n++;
    }

    if (n == 0) return;

    struct LayoutWindow* lw = calloc(n, sizeof(*lw));
    if (lw == NULL) return;

    size_t i = 0;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->output != output) continue;
        if (!window_is_layoutable(w)) continue;
        lw[i++].window = w;
    }

    if (i != n) { // TODO maybe maybe
        free(lw);
        return;
    }

    struct LayoutParams lp = {
        .x = ax,
        .y = ay,
        .width = aw,
        .height = ah,
        .gap_outer_h = wm.tiled_gap_outer_h,
        .gap_outer_v = wm.tiled_gap_outer_v,
        .gap_inner_h = wm.tiled_gap_inner_h,
        .gap_inner_v = wm.tiled_gap_inner_v,
        .nmasters = wm.nmasters,
        .mfact = wm.mfact,
        .smart_gaps = wm.smart_gaps,
        .center_overspread = wm.center_overspread,
        .center_when_single_stack = wm.center_when_single_stack,
        .focused = layout_focused_window_for(output),
    };

    size_t written = compute(wm.layout, lw, n, &lp);
    if (written == 0) {
        free(lw);
        return;
    }

    if (written > n) written = n;

    for (size_t j = 0; j < written; j++) {
        struct Window* fw = lw[j].window;
        if (fw->fullscreen) {
            window_apply_target(
                fw, output->pos_x, output->pos_y, output->width, output->height
            );
        } else {
            window_apply_target(fw, lw[j].x, lw[j].y, lw[j].w, lw[j].h);
        }
    }

    free(lw);

    wl_list_for_each(w, &wm.windows, link) {
        if (w->output != output) continue;
        if (w->closed || w->space_hidden || !w->mapped || w->fullscreen)
            continue;
        if (!window_is_transient(w)) continue;

        struct Window* parent = window_parent(w);
        if (parent == NULL || parent->closed) continue;

        int32_t ww = w->width > 0 ? w->width : 1;
        int32_t wh = w->height > 0 ? w->height : 1;

        window_apply_target(w, parent->x, parent->y, ww, wh);
    }
}

static void floating_layout(
    struct LayoutWindow* lw, size_t n, const struct LayoutParams* p
) {
    bool occupied = false;
    for (size_t i = 0; i < n; i++) {
        if (lw[i].window->pos_valid) {
            occupied = true;
            break;
        }
    }

    if (!occupied) cascade_next_slot = 0;

    for (size_t i = 0; i < n; i++) {
        struct Window* w = lw[i].window;

        if (w->maximized) {
            lw[i].x = p->x;
            lw[i].y = p->y;
            lw[i].w = p->width;
            lw[i].h = p->height;
            continue;
        }

        if (w->anim.running && w->has_target) {
            lw[i].x = w->target_box.x;
            lw[i].y = w->target_box.y;
            lw[i].w = w->target_box.width;
            lw[i].h = w->target_box.height;
            continue;
        }

        int32_t ww = w->width > 0 ? w->width : 1;
        int32_t wh = w->height > 0 ? w->height : 1;

        if (!w->pos_valid) {
            int32_t idx = cascade_next_slot % CFG_FLOATING_CASCADE_MAX;
            cascade_next_slot++;

            int32_t cw = ww < p->width ? ww : p->width;
            int32_t ch = wh < p->height ? wh : p->height;
            int32_t cx = p->x + (p->width - cw) / 2;
            int32_t cy = p->y + (p->height - ch) / 2;

            lw[i].x = cx + CFG_FLOATING_CASCADE_STEP * idx;
            lw[i].y = cy + CFG_FLOATING_CASCADE_STEP * idx;
            lw[i].w = cw;
            lw[i].h = ch;
            continue;
        }

        if (w->anim.running) {
            lw[i].x = w->saved_x;
            lw[i].y = w->saved_y;
        } else {
            lw[i].x = w->x;
            lw[i].y = w->y;
        }

        lw[i].w = ww;
        lw[i].h = wh;
    }
}

static size_t layouts_compute(
    enum Layout layout, struct LayoutWindow* out, size_t n,
    const struct LayoutParams* p
) {
    if (n == 0 || p == NULL || out == NULL) { return 0; }

    switch (layout) {
        case LAYOUT_VERTICAL_TILE: vertical_tile(out, n, p); return n;
        case LAYOUT_VERTICAL_GRID: vertical_grid(out, n, p); return n;
        case LAYOUT_HORIZONTAL_TILE: horizontal_tile(out, n, p); return n;
        case LAYOUT_HORIZONTAL_RIGHT_TILE:
            horizontal_right_tile(out, n, p);
            return n;

        case LAYOUT_HORIZONTAL_MONOCLE: horizontal_monocle(out, n, p); return n;
        case LAYOUT_HORIZONTAL_GRID: horizontal_grid(out, n, p); return n;
        case LAYOUT_FLOATING: floating_layout(out, n, p); return n;
    }

    return 0;
}

void layout_apply(void) {
    if (server.overview_active) return;

    struct Output* out;
    wl_list_for_each(out, &server.outputs, link) {
        if (out->removed) continue;
        layout_output(out, layouts_compute);
    }
}

bool layouts_is_tiled(enum Layout layout) {
    switch (layout) {
        case LAYOUT_VERTICAL_TILE:
        case LAYOUT_VERTICAL_GRID:
        case LAYOUT_HORIZONTAL_TILE:
        case LAYOUT_HORIZONTAL_RIGHT_TILE:
        case LAYOUT_HORIZONTAL_MONOCLE:
        case LAYOUT_HORIZONTAL_GRID: return true;

        case LAYOUT_FLOATING: return false;
    }

    return false;
}
