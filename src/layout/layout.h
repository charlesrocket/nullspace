#pragma once

#include "../window.h"

#include <nullspace/layouts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

typedef size_t (*LayoutCompute)(enum Layout, struct LayoutWindow *, size_t, const struct LayoutParams *);

void layout_apply(void);

static inline float
layout_weight_sum(const struct LayoutWindow *lw, size_t start, size_t n) {
    float s = 0.0f;
    for (size_t i = start; i < start + n; i++) {
        float w = lw[i].window->size_per;
        if (w < 0.1f) w = 0.1f;
        s += w;
    }

    return s > 0.0f ? s : (float)n;
}
