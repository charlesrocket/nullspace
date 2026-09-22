#ifndef LAYOUTS_VERTICAL_H
#define LAYOUTS_VERTICAL_H

#include "layout.h"

void vertical_tile(
    struct LayoutWindow *lw, size_t n, const struct LayoutParams *p
);
void vertical_grid(
    struct LayoutWindow *lw, size_t n, const struct LayoutParams *p
);

#endif // LAYOUTS_VERTICAL_H
