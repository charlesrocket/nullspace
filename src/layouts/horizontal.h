#pragma once

#include "layout.h"

#include <stddef.h>

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
