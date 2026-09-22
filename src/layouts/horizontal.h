#ifndef LAYOUTS_HORIZONTAL_H
#define LAYOUTS_HORIZONTAL_H

#include "layout.h"

void horizontal_tile(
    struct LayoutWindow *lw, size_t n, const struct LayoutParams *p
);

void horizontal_right_tile(
    struct LayoutWindow *lw, size_t n, const struct LayoutParams *p
);

void horizontal_monocle(
    struct LayoutWindow *lw, size_t n, const struct LayoutParams *p
);

void horizontal_grid(
    struct LayoutWindow *lw, size_t n, const struct LayoutParams *p
);

#endif // LAYOUTS_HORIZONTAL_H
