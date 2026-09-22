#include "vertical.h"

#include "layout.h"

static struct LayoutParams
effective_params(const struct LayoutParams *p, size_t n) {
    struct LayoutParams e = *p;
    if (e.smart_gaps && n == 1) {
        e.gap_outer_h = e.gap_outer_v = 0;
        e.gap_inner_h = e.gap_inner_v = 0;
    }

    return e;
}

// Split a horizontal strip into `count` cells. The last cell absorbs the
// rounding remainder so the strip ends exactly at x0 + width.
static void strip_h(
    struct LayoutWindow *lw, size_t start, size_t count, int32_t x0, int32_t y,
    int32_t width, int32_t height, int32_t gap
) {
    if (count == 0) { return; }

    int32_t end = x0 + width;
    int32_t x = x0;

    for (size_t i = 0; i < count; i++) {
        int32_t w;
        if (i == count - 1) {
            w = end - x;
        } else {
            int32_t remaining_cells = (int32_t)(count - i);
            int32_t avail = end - x - (remaining_cells - 1) * gap;
            w = avail / remaining_cells;
        }

        if (w < 1) { w = 1; }

        lw[start + i].x = x;
        lw[start + i].y = y;
        lw[start + i].w = w;
        lw[start + i].h = height;

        x += w + gap;
    }
}

// Master/stack vertical split. Returns the
// virtual master boundary measured from p->y.
static int32_t
master_boundary(const struct LayoutParams *g, int32_t inner_h, size_t nstack) {
    if (nstack == 0) { return g->height - 2 * g->gap_outer_v + g->gap_inner_v; }

    return (int32_t)((float)(g->height + g->gap_inner_v) * g->mfact);
}

void vertical_tile(
    struct LayoutWindow *lw, size_t n, const struct LayoutParams *p
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

    int32_t mh = master_boundary(&g, inner_h, nstack);

    int32_t master_h = mh - g.gap_inner_v;
    if (master_h < 1) { master_h = 1; }

    strip_h(
        lw, 0, nmasters, g.x + g.gap_outer_h, g.y + g.gap_outer_v, inner_w,
        master_h, g.gap_inner_h
    );

    if (nstack == 0) { return; }

    int32_t stack_y = g.y + g.gap_outer_v + mh;
    int32_t stack_h = g.height - mh - 2 * g.gap_outer_v;

    if (stack_h < 1) { stack_h = 1; }

    strip_h(
        lw, nmasters, nstack, g.x + g.gap_outer_h, stack_y, inner_w, stack_h,
        g.gap_inner_h
    );
}

void vertical_grid(
    struct LayoutWindow *lw, size_t n, const struct LayoutParams *p
) {
    struct LayoutParams g = effective_params(p, n);

    size_t rows = 1;
    while (rows * rows < n) { rows++; }

    size_t base_cols = n / rows;
    size_t remainder = n % rows;
    size_t first_group_rows = rows - remainder;
    size_t first_group_count = first_group_rows * base_cols;
    size_t max_cols = base_cols + (remainder > 0 ? 1 : 0);

    int32_t inner_w = g.width - 2 * g.gap_outer_h;
    int32_t inner_h = g.height - 2 * g.gap_outer_v;
    if (inner_w < 1) { inner_w = 1; }
    if (inner_h < 1) { inner_h = 1; }

    int32_t row_h =
        (inner_h - (int32_t)(rows - 1) * g.gap_inner_v) / (int32_t)rows;
    if (row_h < 1) { row_h = 1; }

    int32_t base_col_w = 0;
    if (base_cols > 0) {
        base_col_w = (inner_w - (int32_t)(base_cols - 1) * g.gap_inner_h)
                   / (int32_t)base_cols;
        if (base_col_w < 1) { base_col_w = 1; }
    }

    int32_t max_col_w = 0;
    if (max_cols > 0) {
        max_col_w = (inner_w - (int32_t)(max_cols - 1) * g.gap_inner_h)
                  / (int32_t)max_cols;
        if (max_col_w < 1) { max_col_w = 1; }
    }

    int32_t group_x = g.x + g.gap_outer_h;
    int32_t group_y = g.y + g.gap_outer_v;
    int32_t bottom = g.y + g.gap_outer_v + inner_h;
    int32_t right = g.x + g.gap_outer_h + inner_w;

    for (size_t i = 0; i < n; i++) {
        size_t row_idx, col_idx, cols_in_row;
        int32_t col_w;

        if (i < first_group_count) {
            row_idx = i / base_cols;
            col_idx = i % base_cols;
            cols_in_row = base_cols;
            col_w = base_col_w;
        } else {
            size_t offset = i - first_group_count;
            row_idx = first_group_rows + offset / max_cols;
            col_idx = offset % max_cols;
            cols_in_row = max_cols;
            col_w = max_col_w;
        }

        int32_t x = group_x + (int32_t)col_idx * (col_w + g.gap_inner_h);
        int32_t y = group_y + (int32_t)row_idx * (row_h + g.gap_inner_v);

        lw[i].x = x;
        lw[i].y = y;
        lw[i].w = (col_idx == cols_in_row - 1) ? (right - x) : col_w;
        lw[i].h = (row_idx == rows - 1) ? (bottom - y) : row_h;
    }
}
