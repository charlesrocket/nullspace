#pragma once

#include "param.h"

#include <stddef.h>

void vertical_tile(
    struct LayoutWindow* lw, size_t n, const struct LayoutParams* p
);
void vertical_grid(
    struct LayoutWindow* lw, size_t n, const struct LayoutParams* p
);
