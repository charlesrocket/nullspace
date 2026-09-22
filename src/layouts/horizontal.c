#include "horizontal.h"

#include "layout.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

static inline int32_t hmin(int32_t a, int32_t b) { return a < b ? a : b; }

// Smart_gaps zeroes everything when there is a single window,
// matching the current vertical layouts.
static void effective_gaps(
    const struct LayoutParams *p, size_t n, int32_t *go_h, int32_t *go_v,
    int32_t *gi_h, int32_t *gi_v
) {
    *go_h = p->gap_outer_h;
    *go_v = p->gap_outer_v;
    *gi_h = p->gap_inner_h;
    *gi_v = p->gap_inner_v;

    if (p->smart_gaps && n == 1) { *go_h = *go_v = *gi_h = *gi_v = 0; }
}

// Master column on the left, stack column on the right.
void horizontal_tile(
    struct LayoutWindow *lw, size_t n, const struct LayoutParams *p
) {
    int32_t go_h, go_v, gi_h, gi_v;
    effective_gaps(p, n, &go_h, &go_v, &gi_h, &gi_v);

    int32_t count = (int32_t)n;
    int32_t nm = hmin(p->nmasters, count);

    int32_t mw;
    if (count > nm) {
        mw = nm ? (int32_t)((float)(p->width + gi_h) * p->mfact) : 0;
    } else {
        mw = p->width - 2 * go_h + gi_h;
    }

    int32_t my = go_v;
    int32_t ty = go_v;

    for (int32_t i = 0; i < count; i++) {
        if (i < nm) {
            int32_t r = nm - i;
            int32_t h = (p->height - my - go_v - gi_v * (r - 1)) / r;
            lw[i].x = p->x + go_h;
            lw[i].y = p->y + my;
            lw[i].w = mw - gi_h;
            lw[i].h = h;
            my += h + gi_v;
        } else {
            int32_t r = count - i;
            int32_t h = (p->height - ty - go_v - gi_v * (r - 1)) / r;
            lw[i].x = p->x + mw + go_h;
            lw[i].y = p->y + ty;
            lw[i].w = p->width - mw - 2 * go_h;
            lw[i].h = h;
            ty += h + gi_v;
        }
    }
}

// Master column on the right, stack on the left.
void horizontal_right_tile(
    struct LayoutWindow *lw, size_t n, const struct LayoutParams *p
) {
    int32_t go_h, go_v, gi_h, gi_v;
    effective_gaps(p, n, &go_h, &go_v, &gi_h, &gi_v);

    int32_t count = (int32_t)n;
    int32_t nm = hmin(p->nmasters, count);

    int32_t mw;
    if (count > nm) {
        mw = nm ? (int32_t)((float)(p->width + gi_h) * p->mfact) : 0;
    } else {
        mw = p->width - 2 * go_h + gi_h;
    }

    int32_t my = go_v;
    int32_t ty = go_v;

    for (int32_t i = 0; i < count; i++) {
        if (i < nm) {
            int32_t r = nm - i;
            int32_t h = (p->height - my - go_v - gi_v * (r - 1)) / r;
            lw[i].x = p->x + p->width - mw - go_h + gi_h;
            lw[i].y = p->y + my;
            lw[i].w = mw - gi_h;
            lw[i].h = h;
            my += h + gi_v;
        } else {
            int32_t r = count - i;
            int32_t h = (p->height - ty - go_v - gi_v * (r - 1)) / r;
            lw[i].x = p->x + go_h;
            lw[i].y = p->y + ty;
            lw[i].w = p->width - mw - 2 * go_h;
            lw[i].h = h;
            ty += h + gi_v;
        }
    }
}

// The focused window is centered in the middle of the screen. All unfocused
// windows are shown as a row of previews below. Focus promotes the window to
// the center.
void horizontal_monocle(
    struct LayoutWindow *lw, size_t n, const struct LayoutParams *p
) {
    int32_t go_h, go_v, gi_h, gi_v;
    effective_gaps(p, n, &go_h, &go_v, &gi_h, &gi_v);

    // Find which entry is focused (default to the first window).
    size_t focus_idx = 0;
    bool have_focus = false;
    if (p->focused != NULL) {
        for (size_t i = 0; i < n; i++) {
            if (lw[i].window == p->focused) {
                focus_idx = i;
                have_focus = true;
                break;
            }
        }
    }

    if (!have_focus) { focus_idx = 0; }

    if (n <= 1) {
        lw[focus_idx].x = p->x + go_h;
        lw[focus_idx].y = p->y + go_v;
        lw[focus_idx].w = p->width - 2 * go_h;
        lw[focus_idx].h = p->height - 2 * go_v;
        return;
    }

    // Unfocused windows row (20%)
    int32_t preview_h = (int32_t)((float)p->height * 0.20f);
    int32_t preview_y = p->y + p->height - go_v - preview_h;

    // Focused window
    lw[focus_idx].x = p->x + go_h;
    lw[focus_idx].y = p->y + go_v;
    lw[focus_idx].w = p->width - 2 * go_h;
    lw[focus_idx].h = preview_y - gi_v - (p->y + go_v);

    int32_t count = (int32_t)(n - 1);
    int32_t avail_w = p->width - 2 * go_h - gi_h * (count - 1);
    int32_t unit_w = avail_w / count;

    int32_t next_x = p->x + go_h;
    int32_t placed = 0;

    for (size_t i = 0; i < n; i++) {
        if (i == focus_idx) { continue; }

        int32_t w =
            (placed == count - 1) ? (p->x + p->width - go_h - next_x) : unit_w;

        lw[i].x = next_x;
        lw[i].y = preview_y;
        lw[i].w = w;
        lw[i].h = preview_h;

        next_x += w + gi_h;
        placed++;
    }
}

void horizontal_grid(
    struct LayoutWindow *lw, size_t n, const struct LayoutParams *p
) {
    int32_t go_h, go_v, gi_h, gi_v;
    effective_gaps(p, n, &go_h, &go_v, &gi_h, &gi_v);
    int32_t count = (int32_t)n;

    int32_t cols = 0;
    while (cols * cols < count) cols++;

    int32_t base_rows = count / cols;
    int32_t remainder = count % cols;
    int32_t first_group_cols = cols - remainder;
    int32_t first_group_count = first_group_cols * base_rows;
    int32_t max_rows = base_rows + (remainder > 0 ? 1 : 0);

    int32_t avail_w = p->width - 2 * go_h - (cols - 1) * gi_h;

    int32_t *col_x = malloc(sizeof(int32_t) * (size_t)cols);
    int32_t *col_w = malloc(sizeof(int32_t) * (size_t)cols);

    if (col_x == NULL || col_w == NULL) {
        free(col_x);
        free(col_w);
        return;
    }

    int32_t unit_w = avail_w / cols;
    int32_t next_x = p->x + go_h;

    for (int32_t i = 0; i < cols; i++) {
        col_x[i] = next_x;
        col_w[i] = (i == cols - 1) ? (p->x + p->width - go_h - next_x) : unit_w;
        next_x += col_w[i] + gi_h;
    }

    int32_t avail_h_base = p->height - 2 * go_v - (base_rows - 1) * gi_v;
    int32_t *row_y_base = malloc(sizeof(int32_t) * (size_t)base_rows);
    int32_t *row_h_base = malloc(sizeof(int32_t) * (size_t)base_rows);

    if (row_y_base == NULL || row_h_base == NULL) {
        free(col_x);
        free(col_w);
        free(row_y_base);
        free(row_h_base);
        return;
    }

    int32_t unit_h_base = avail_h_base / base_rows;
    int32_t next_y = p->y + go_v;
    for (int32_t i = 0; i < base_rows; i++) {
        row_y_base[i] = next_y;
        row_h_base[i] = (i == base_rows - 1)
                          ? (p->y + p->height - go_v - next_y)
                          : unit_h_base;
        next_y += row_h_base[i] + gi_v;
    }

    int32_t *row_y_max = NULL;
    int32_t *row_h_max = NULL;
    if (remainder > 0) {
        row_y_max = malloc(sizeof(int32_t) * (size_t)max_rows);
        row_h_max = malloc(sizeof(int32_t) * (size_t)max_rows);
        if (row_y_max == NULL || row_h_max == NULL) {
            free(col_x);
            free(col_w);
            free(row_y_base);
            free(row_h_base);
            free(row_y_max);
            free(row_h_max);
            return;
        }

        int32_t avail_h_max = p->height - 2 * go_v - (max_rows - 1) * gi_v;
        int32_t unit_h_max = avail_h_max / max_rows;
        next_y = p->y + go_v;
        for (int32_t i = 0; i < max_rows; i++) {
            row_y_max[i] = next_y;
            row_h_max[i] = (i == max_rows - 1)
                             ? (p->y + p->height - go_v - next_y)
                             : unit_h_max;
            next_y += row_h_max[i] + gi_v;
        }
    }

    for (int32_t i = 0; i < count; i++) {
        int32_t col_idx, row_idx, cy, ch;

        if (i < first_group_count) {
            col_idx = i / base_rows;
            row_idx = i % base_rows;
            cy = row_y_base[row_idx];
            ch = row_h_base[row_idx];
        } else {
            int32_t offset = i - first_group_count;
            col_idx = first_group_cols + (offset / max_rows);
            row_idx = offset % max_rows;
            cy = row_y_max[row_idx];
            ch = row_h_max[row_idx];
        }

        lw[i].x = col_x[col_idx];
        lw[i].y = cy;
        lw[i].w = col_w[col_idx];
        lw[i].h = ch;
    }

    free(col_x);
    free(col_w);
    free(row_y_base);
    free(row_h_base);
    free(row_y_max);
    free(row_h_max);
}
