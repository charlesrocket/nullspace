#ifndef LAYOUT_H
#define LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

enum Layout {
    LAYOUT_TRIMMING,
    LAYOUT_VERTICAL_TILE,
    LAYOUT_VERTICAL_GRID,

    LAYOUT_HORIZONTAL_TILE,
    LAYOUT_HORIZONTAL_RIGHT_TILE,
    LAYOUT_HORIZONTAL_MONOCLE,
    LAYOUT_HORIZONTAL_GRID,

    LAYOUT_FLOATING,
    LAYOUT_LAST = LAYOUT_FLOATING,
};

struct Window;

struct LayoutWindow {
    struct Window *window;
    int32_t x, y, w, h;
};

struct LayoutParams {
    // Currently focused window, or NULL if none/unknown
    // (layouts that treat the focused window specially (monocle)
    // fall back to `out[0]` when this is NULL).
    struct Window *focused;

    int32_t x, y, width, height; // usable area (exclusive zone)
    int32_t gap_outer_h, gap_outer_v;
    int32_t gap_inner_h, gap_inner_v;
    int32_t nmasters; // tile
    float mfact;      // tile

    bool smart_gaps;
    bool center_overspread, center_when_single_stack;
};

bool layouts_is_tiled(enum Layout layout);

void layout_tiled_apply(const struct timespec *now);

// Fill out[0..n-1] with target rects for `layout`. Every out[i].window must
// already be set; only .x/.y/.w/.h are written. Returns the number of
// entries filled, or 0 if the layout is not handled here (trimming,
// floating).
size_t layouts_compute(
    enum Layout layout, struct LayoutWindow *out, size_t n,
    const struct LayoutParams *p
);

typedef size_t (*LayoutCompute)(enum Layout, struct LayoutWindow *, size_t, const struct LayoutParams *);

void layout_apply(const struct timespec *now);
void layout_vertical_apply(const struct timespec *now);
void layout_horizontal_apply(const struct timespec *now);

#endif // LAYOUT_H
