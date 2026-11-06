#include "vertical.h"

#include "../window.h"
#include "layout.h"

#include <stddef.h>
#include <stdint.h>

static struct LayoutParams
effective_params(const struct LayoutParams* p, size_t n) {
    struct LayoutParams e = *p;
    if (e.smart_gaps && n == 1) {
        e.gap_outer_h = e.gap_outer_v = 0;
        e.gap_inner_h = e.gap_inner_v = 0;
    }

    return e;
}

static float weight_of(const struct Window* w) {
    if (w == NULL) { return 1.0f; }

    float wp = w->size_per;
    if (!(wp >= 0.1f)) { return 0.1f; }

    return wp;
}

static void strip_h_weighted(
    struct LayoutWindow* lw, size_t start, size_t count, int32_t x0, int32_t y,
    int32_t width, int32_t height, int32_t gap
) {
    if (count == 0) { return; }

    float total = 0.0f;
    for (size_t i = 0; i < count; i++) {
        total += weight_of(lw[start + i].window);
    }

    if (total <= 0.0f) { total = (float)count; }

    const int32_t end = x0 + width;
    int32_t x = x0;
    float remaining = total;

    for (size_t i = 0; i < count; i++) {
        const float wp = weight_of(lw[start + i].window);
        int32_t w;

        if (i == count - 1) {
            w = end - x;
        } else {
            int32_t avail_here = end - x - (int32_t)(count - i - 1) * gap;

            if (avail_here < 1) { avail_here = 1; }

            w = (int32_t)((float)avail_here * (wp / remaining));

            int32_t min_rest = (int32_t)(count - i - 1);
            if (w > avail_here - min_rest) { w = avail_here - min_rest; }
        }

        if (w < 1) { w = 1; }

        lw[start + i].x = x;
        lw[start + i].y = y;
        lw[start + i].w = w;
        lw[start + i].h = height;

        x += w + gap;
        remaining -= wp;
        if (remaining < 0.0f) { remaining = 0.0f; }
    }
}

static int32_t master_boundary(const struct LayoutParams* g, size_t nstack) {
    if (nstack == 0) { return g->height - 2 * g->gap_outer_v + g->gap_inner_v; }

    return (int32_t)((float)(g->height + g->gap_inner_v) * g->mfact);
}

void vertical_tile(
    struct LayoutWindow* lw, size_t n, const struct LayoutParams* p
) {
    struct LayoutParams g = effective_params(p, n);

    size_t nmasters = (size_t)g.nmasters;
    if (nmasters < 1) { nmasters = 1; }
    if (nmasters > n) { nmasters = n; }

    size_t nstack = n - nmasters;

    int32_t inner_w = g.width - 2 * g.gap_outer_h;
    int32_t inner_h = g.height - 2 * g.gap_outer_v;
    if (inner_w < 1) { inner_w = 1; }
    if (inner_h < 1) { inner_h = 1; }

    int32_t mh = master_boundary(&g, nstack);
    int32_t master_h = mh - g.gap_inner_v;

    if (master_h < 1) { master_h = 1; }

    strip_h_weighted(
        lw, 0, nmasters, g.x + g.gap_outer_h, g.y + g.gap_outer_v, inner_w,
        master_h, g.gap_inner_h
    );

    if (nstack == 0) { return; }

    int32_t stack_y = g.y + g.gap_outer_v + mh;
    int32_t stack_h = g.height - mh - 2 * g.gap_outer_v;

    if (stack_h < 1) { stack_h = 1; }

    strip_h_weighted(
        lw, nmasters, nstack, g.x + g.gap_outer_h, stack_y, inner_w, stack_h,
        g.gap_inner_h
    );
}

void vertical_grid(
    struct LayoutWindow* lw, size_t n, const struct LayoutParams* p
) {
    if (n == 0) { return; }
    struct LayoutParams g = effective_params(p, n);

    size_t rows = 1;
    while (rows * rows < n) { rows++; }

    size_t base_cols = n / rows;
    size_t remainder = n % rows;
    size_t first_group_rows = rows - remainder;
    size_t first_group_count = first_group_rows * base_cols;
    size_t max_cols = base_cols + (remainder > 0 ? 1 : 0);
    if (base_cols == 0 || max_cols == 0) { return; }

    int32_t inner_w = g.width - 2 * g.gap_outer_h;
    int32_t inner_h = g.height - 2 * g.gap_outer_v;
    if (inner_w < 1) { inner_w = 1; }
    if (inner_h < 1) { inner_h = 1; }

    int32_t row_h =
        (inner_h - (int32_t)(rows - 1) * g.gap_inner_v) / (int32_t)rows;

    if (row_h < 1) { row_h = 1; }

    int32_t group_x = g.x + g.gap_outer_h;
    int32_t group_y = g.y + g.gap_outer_v;
    int32_t bottom = g.y + g.gap_outer_v + inner_h;
    int32_t right = g.x + g.gap_outer_h + inner_w;

    for (size_t r = 0; r < rows; r++) {
        size_t start, count;
        if (r < first_group_rows) {
            start = r * base_cols;
            count = base_cols;
        } else {
            start = first_group_count + (r - first_group_rows) * max_cols;
            count = max_cols;
        }

        float total = 0.0f;
        for (size_t k = 0; k < count; k++) {
            total += weight_of(lw[start + k].window);
        }

        if (total <= 0.0f) { total = (float)count; }

        int32_t row_y = group_y + (int32_t)r * (row_h + g.gap_inner_v);
        int32_t rh = (r == rows - 1) ? (bottom - row_y) : row_h;

        int32_t cell_x = group_x;
        float remaining = total;

        for (size_t k = 0; k < count; k++) {
            size_t i = start + k;
            float wp = weight_of(lw[i].window);
            int32_t cw;

            if (k == count - 1) {
                cw = right - cell_x;
            } else {
                int32_t avail_here =
                    right - cell_x - (int32_t)(count - k - 1) * g.gap_inner_h;

                if (avail_here < 1) { avail_here = 1; }

                cw = (int32_t)((float)avail_here * (wp / remaining));

                int32_t min_rest = (int32_t)(count - k - 1);
                if (cw > avail_here - min_rest) { cw = avail_here - min_rest; }
            }

            if (cw < 1) { cw = 1; }

            lw[i].x = cell_x;
            lw[i].y = row_y;
            lw[i].w = cw;
            lw[i].h = rh;

            cell_x += cw + g.gap_inner_h;
            remaining -= wp;
            if (remaining < 0.0f) { remaining = 0.0f; }
        }
    }
}
