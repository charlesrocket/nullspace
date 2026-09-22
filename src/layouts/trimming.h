#ifndef TRIMMING_H
#define TRIMMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct TrimmingTree;

struct TrimmingConfig {
    float split_ratio;

    int hsplit; // 0 = auto (cursor), 1 = second, 2 = first
    int vsplit;

    bool manual_split;
    bool preserve_split;
    bool smart_split;
};

struct TrimmingPlacement {
    int32_t x, y, width, height;
    void *handle;
};

struct TrimmingLayoutParams {
    int32_t x, y, width, height;      // outer output rectangle
    int32_t gap_outer_h, gap_outer_v; // reserved at the outside edges
    int32_t gap_inner_h, gap_inner_v; // reserved between split children
};

struct TrimmingTree *trimming_create(void);
void trimming_destroy(struct TrimmingTree *t);

void trimming_set_config(
    struct TrimmingTree *t, const struct TrimmingConfig *cfg
);

void trimming_insert(
    struct TrimmingTree *t, void *client, void *focused, int32_t cursor_x,
    int32_t cursor_y, int32_t focus_x, int32_t focus_y, int32_t focus_w,
    int32_t focus_h
);

void trimming_remove(struct TrimmingTree *t, void *client);

size_t trimming_leaf_count(const struct TrimmingTree *t);

size_t trimming_layout(
    struct TrimmingTree *t, const struct TrimmingLayoutParams *p,
    struct TrimmingPlacement *out, size_t cap
);

size_t trimming_assign(
    struct TrimmingTree *t, int32_t x, int32_t y, int32_t width, int32_t height,
    int32_t gap_inner_h, int32_t gap_inner_v, struct TrimmingPlacement *out,
    size_t cap
);

#endif /* TRIMMING_H */
