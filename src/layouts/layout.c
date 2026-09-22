#include "layout.h"

#include "../nullspace.h"
#include "horizontal.h"
#include "trimming.h"
#include "vertical.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

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

static void
layout_windows_apply(const struct timespec *now, LayoutCompute compute) {
    struct Output *output = tiling_output();
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
        window_apply_target(
            lw[j].window, lw[j].x, lw[j].y, lw[j].w, lw[j].h, now
        );
    }

    free(lw);
}

void layout_tiled_apply(const struct timespec *now) {
    struct Output *output = tiling_output();
    if (output == NULL) { return; }

    // Tile only within the exclusive zone
    int32_t area_x, area_y, out_w, out_h;
    output_usable_area(output, &area_x, &area_y, &out_w, &out_h);
    if (out_w <= 0 || out_h <= 0) { return; }

    trimming_sync(area_x, area_y, out_w, out_h);

    size_t cap = trimming_leaf_count(wm.trimming_tree);
    if (cap == 0) { return; }

    struct TrimmingPlacement *placements =
        calloc(cap, sizeof(struct TrimmingPlacement));
    if (placements == NULL) { return; }

    struct TrimmingLayoutParams lp = {
        .x = area_x,
        .y = area_y,
        .width = out_w,
        .height = out_h,
        .gap_outer_h = wm.tiled_gap_outer_h,
        .gap_outer_v = wm.tiled_gap_outer_v,
        .gap_inner_h = wm.tiled_gap_inner_h,
        .gap_inner_v = wm.tiled_gap_inner_v,
    };

    size_t n = trimming_layout(wm.trimming_tree, &lp, placements, cap);

    for (size_t i = 0; i < n; i++) {
        struct Window *w = placements[i].handle;
        window_apply_target(
            w, placements[i].x, placements[i].y, placements[i].width,
            placements[i].height, now
        );
    }

    free(placements);
}

void layout_apply(const struct timespec *now) {
    switch (wm.layout) {
        case LAYOUT_TRIMMING: layout_tiled_apply(now); break;

        case LAYOUT_FLOATING: break;

        default: layout_windows_apply(now, layouts_compute); break;
    }
}

bool layouts_is_tiled(enum Layout layout) {
    switch (layout) {
        case LAYOUT_TRIMMING:
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

        case LAYOUT_TRIMMING:
        case LAYOUT_FLOATING: return 0;
    }

    return 0;
}
