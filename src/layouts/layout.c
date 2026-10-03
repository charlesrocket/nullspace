#include "layout.h"

#include "../manager.h"
#include "../output.h"
#include "horizontal.h"
#include "vertical.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <wayland-util.h>

static void output_usable_area(
    struct Output *output, int32_t *x, int32_t *y, int32_t *w, int32_t *h
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

static struct Window *layout_focused_window(void) {
    struct Window *window;
    wl_list_for_each_reverse(window, &wm.focus_stack, focus_link) {
        if (window->closed) { continue; }
        if (window->space_hidden) { continue; }
        return window;
    }

    return NULL;
}

static void layout_windows_apply(LayoutCompute compute) {
    struct Output *output = output_primary();
    if (output == NULL) { return; }

    int32_t ax, ay, aw, ah;
    output_usable_area(output, &ax, &ay, &aw, &ah);
    if (aw <= 0 || ah <= 0) { return; }

    size_t n = 0;
    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (!w->closed && !w->space_hidden) { n++; }
    }

    if (n == 0) { return; }

    struct LayoutWindow *lw = calloc(n, sizeof(*lw));
    if (lw == NULL) { return; }

    size_t i = 0;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->closed || w->space_hidden) { continue; }
        lw[i++].window = w;
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
        .focused = layout_focused_window(),
    };

    size_t written = compute(wm.layout, lw, n, &lp);
    if (written == 0) {
        free(lw);
        return;
    }

    for (size_t j = 0; j < written; j++) {
        window_apply_target(lw[j].window, lw[j].x, lw[j].y, lw[j].w, lw[j].h);
    }

    free(lw);
}

void layout_apply(void) { layout_windows_apply(layouts_compute); }

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

static void floating_layout(
    struct LayoutWindow *lw, size_t n, const struct LayoutParams *p
) {
    for (size_t i = 0; i < n; i++) {
        struct Window *w = lw[i].window;

        if (w->anim.running) {
            lw[i].x = w->saved_x;
            lw[i].y = w->saved_y;
        } else {
            lw[i].x = w->x;
            lw[i].y = w->y;
        }

        lw[i].w = w->width > 0 ? w->width : 1;
        lw[i].h = w->height > 0 ? w->height : 1;
    }
}

size_t layouts_compute(
    enum Layout layout, struct LayoutWindow *out, size_t n,
    const struct LayoutParams *p
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
