#ifndef LAYOUTS_H
#define LAYOUTS_H

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

#define LAYOUTS_TABLE(X)                                                       \
    X(LAYOUT_VERTICAL_TILE, "vtile",                                           \
      "vertical tile: master column on top, stack below")                      \
    X(LAYOUT_VERTICAL_GRID, "vgrid", "vertical grid")                          \
    X(LAYOUT_HORIZONTAL_TILE, "htile",                                         \
      "horizontal tile: master column to the left, stack to the right")        \
    X(LAYOUT_HORIZONTAL_RIGHT_TILE, "hrtile",                                  \
      "horizontal right tile: master column to the right, stack to he left")   \
    X(LAYOUT_HORIZONTAL_MONOCLE, "monocle",                                    \
      "focused window centered, previews below")                               \
    X(LAYOUT_HORIZONTAL_GRID, "hgrid", "horizontal grid")                      \
    X(LAYOUT_FLOATING, "float", "floating windows")

#endif // LAYOUTS_H
