#ifndef LAYOUT_H
#define LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum Layout {
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

size_t layouts_compute(
    enum Layout layout, struct LayoutWindow *out, size_t n,
    const struct LayoutParams *p
);

typedef size_t (*LayoutCompute)(enum Layout, struct LayoutWindow *, size_t, const struct LayoutParams *);

void layout_apply(void);

#endif // LAYOUT_H
