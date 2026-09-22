#include "trimming.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct TrimmingPlacement
find_by_handle(struct TrimmingPlacement *ps, size_t n, void *h) {
    for (size_t i = 0; i < n; i++) {
        if (ps[i].handle == h) { return ps[i]; }
    }

    struct TrimmingPlacement zero = {0};
    return zero;
}

static void dump(const char *label, struct TrimmingPlacement *p, size_t n) {
    printf("--- %s ---\n", label);

    for (size_t i = 0; i < n; i++) {
        printf(
            "  [%zu] h=%p x=%4d y=%4d w=%4d h=%4d\n", i, p[i].handle, p[i].x,
            p[i].y, p[i].width, p[i].height
        );
    }
}

static struct TrimmingTree *new_tree(struct TrimmingConfig *cfg) {
    struct TrimmingTree *t = trimming_create();
    struct TrimmingConfig defaults = {0};
    defaults.split_ratio = 0.5f;
    if (cfg != NULL) { defaults = *cfg; }
    trimming_set_config(t, &defaults);
    return t;
}

static void test_single_window(void) {
    struct TrimmingTree *t = new_tree(NULL);
    int a = 1;
    trimming_insert(t, &a, NULL, 0, 0, 0, 0, 0, 0);
    assert(trimming_leaf_count(t) == 1);

    struct TrimmingLayoutParams lp = {
        .x = 0,
        .y = 0,
        .width = 800,
        .height = 600,
        .gap_outer_h = 0,
        .gap_outer_v = 0,
        .gap_inner_h = 0,
        .gap_inner_v = 0,
    };

    struct TrimmingPlacement p[4];
    size_t n = trimming_layout(t, &lp, p, 4);
    assert(n == 1);
    assert(p[0].handle == &a);
    assert(
        p[0].x == 0 && p[0].y == 0 && p[0].width == 800 && p[0].height == 600
    );

    trimming_destroy(t);
}

static void test_outer_gap_single(void) {
    struct TrimmingTree *t = new_tree(NULL);
    int a = 1;
    trimming_insert(t, &a, NULL, 0, 0, 0, 0, 0, 0);

    const int32_t g = 8;
    struct TrimmingLayoutParams lp = {
        .x = 0,
        .y = 0,
        .width = 800,
        .height = 600,
        .gap_outer_h = g,
        .gap_outer_v = g,
        .gap_inner_h = g,
        .gap_inner_v = g,
    };

    struct TrimmingPlacement p[4];
    size_t n = trimming_layout(t, &lp, p, 4);
    assert(n == 1);

    assert(p[0].x == g);
    assert(p[0].y == g);
    assert(p[0].x + p[0].width == 800 - g);
    assert(p[0].y + p[0].height == 600 - g);

    dump("outer gap, single", p, n);
    trimming_destroy(t);
}

static void test_outer_gap_two(void) {
    struct TrimmingTree *t = new_tree(NULL);
    int a = 1, b = 2;

    trimming_insert(t, &a, NULL, 0, 0, 0, 0, 0, 0);
    trimming_insert(t, &b, &a, 500, 300, 8, 8, 784, 584);

    const int32_t g = 8;
    struct TrimmingLayoutParams lp = {
        .x = 0,
        .y = 0,
        .width = 800,
        .height = 600,
        .gap_outer_h = g,
        .gap_outer_v = g,
        .gap_inner_h = g,
        .gap_inner_v = g,
    };

    struct TrimmingPlacement p[4];
    size_t n = trimming_layout(t, &lp, p, 4);
    assert(n == 2);

    struct TrimmingPlacement pa = find_by_handle(p, n, &a);
    struct TrimmingPlacement pb = find_by_handle(p, n, &b);

    dump("outer gap, two", p, n);

    assert(pa.x == g);
    assert(pb.x + pb.width == 800 - g);
    assert(pa.y == g);
    assert(pa.height == 600 - 2 * g);
    assert(pb.y == g);
    assert(pb.height == 600 - 2 * g);
    assert(pb.x == pa.x + pa.width + g);

    trimming_destroy(t);
    puts("outer gap two: ok");
}

static void test_three_windows_dwindle(void) {
    struct TrimmingTree *t = new_tree(NULL);
    int a = 1, b = 2, c = 3;

    trimming_insert(t, &a, NULL, 0, 0, 0, 0, 0, 0);
    trimming_insert(t, &b, &a, 500, 300, 0, 0, 800, 600);
    trimming_insert(t, &c, &b, 600, 300, 400, 0, 400, 600);

    assert(trimming_leaf_count(t) == 3);

    struct TrimmingLayoutParams lp = {
        .x = 0,
        .y = 0,
        .width = 800,
        .height = 600,
        .gap_outer_h = 0,
        .gap_outer_v = 0,
        .gap_inner_h = 0,
        .gap_inner_v = 0,
    };

    struct TrimmingPlacement p[4];
    size_t n = trimming_layout(t, &lp, p, 4);
    assert(n == 3);

    struct TrimmingPlacement pa = find_by_handle(p, n, &a);
    struct TrimmingPlacement pb = find_by_handle(p, n, &b);
    struct TrimmingPlacement pc = find_by_handle(p, n, &c);

    dump("three", p, n);

    assert(pa.x == 0 && pa.y == 0 && pa.width == 400 && pa.height == 600);
    assert(pb.x == 400 && pb.y == 0 && pb.width == 400 && pb.height == 300);
    assert(pc.x == 400 && pc.y == 300 && pc.width == 400 && pc.height == 300);

    trimming_destroy(t);
}

static void test_split_second_leaf(void) {
    struct TrimmingTree *t = new_tree(NULL);
    int a = 1, b = 2, c = 3;

    trimming_insert(t, &a, NULL, 0, 0, 0, 0, 0, 0);
    trimming_insert(t, &b, &a, 500, 300, 0, 0, 800, 600);

    // Now split b (right half), not a
    trimming_insert(t, &c, &b, 600, 300, 400, 0, 400, 600);

    struct TrimmingLayoutParams lp = {
        .x = 0,
        .y = 0,
        .width = 800,
        .height = 600,
    };

    struct TrimmingPlacement p[4];
    size_t n = trimming_layout(t, &lp, p, 4);
    assert(n == 3);

    struct TrimmingPlacement pa = find_by_handle(p, n, &a);
    struct TrimmingPlacement pb = find_by_handle(p, n, &b);
    struct TrimmingPlacement pc = find_by_handle(p, n, &c);

    // a stays whole on the left; b and c stack on the right
    assert(pa.height == 600);
    assert(pb.x == 400 && pb.y == 0);
    assert(pc.x == 400 && pc.y == 300);

    dump("split second leaf", p, n);
    trimming_destroy(t);
}

static void test_remove_promotes_sibling(void) {
    struct TrimmingTree *t = new_tree(NULL);
    int a = 1, b = 2, c = 3;

    trimming_insert(t, &a, NULL, 0, 0, 0, 0, 0, 0);
    trimming_insert(t, &b, &a, 500, 300, 0, 0, 800, 600);
    trimming_insert(t, &c, &b, 600, 300, 400, 0, 400, 600);

    trimming_remove(t, &c);
    assert(trimming_leaf_count(t) == 2);

    struct TrimmingLayoutParams lp = {
        .x = 0,
        .y = 0,
        .width = 800,
        .height = 600,
    };

    struct TrimmingPlacement p[4];
    size_t n = trimming_layout(t, &lp, p, 4);
    assert(n == 2);

    struct TrimmingPlacement pa = find_by_handle(p, n, &a);
    struct TrimmingPlacement pb = find_by_handle(p, n, &b);

    assert(pa.x == 0 && pa.width == 400 && pa.height == 600);
    assert(pb.x == 400 && pb.width == 400 && pb.height == 600);

    dump("after remove c", p, n);
    trimming_destroy(t);
}

static void test_smart_split_top(void) {
    struct TrimmingConfig cfg = {0};
    cfg.smart_split = true;
    cfg.split_ratio = 0.5f;
    struct TrimmingTree *t = new_tree(&cfg);

    int a = 1, b = 2;
    trimming_insert(t, &a, NULL, 0, 0, 0, 0, 0, 0);
    trimming_insert(t, &b, &a, 400, 100, 0, 0, 800, 600);

    struct TrimmingLayoutParams lp = {
        .x = 0,
        .y = 0,
        .width = 800,
        .height = 600,
    };

    struct TrimmingPlacement p[4];
    size_t n = trimming_layout(t, &lp, p, 4);
    assert(n == 2);

    struct TrimmingPlacement pa = find_by_handle(p, n, &a);
    struct TrimmingPlacement pb = find_by_handle(p, n, &b);

    assert(pb.y == 0 && pb.height == 300);
    assert(pa.y == 300 && pa.height == 300);

    dump("smart top", p, n);
    trimming_destroy(t);
}

static void test_smart_split_left(void) {
    struct TrimmingConfig cfg = {0};
    cfg.smart_split = true;
    cfg.split_ratio = 0.5f;
    struct TrimmingTree *t = new_tree(&cfg);

    int a = 1, b = 2;
    trimming_insert(t, &a, NULL, 0, 0, 0, 0, 0, 0);
    trimming_insert(t, &b, &a, 100, 300, 0, 0, 800, 600);

    struct TrimmingLayoutParams lp = {
        .x = 0,
        .y = 0,
        .width = 800,
        .height = 600,
    };

    struct TrimmingPlacement p[4];
    size_t n = trimming_layout(t, &lp, p, 4);
    assert(n == 2);

    struct TrimmingPlacement pa = find_by_handle(p, n, &a);
    struct TrimmingPlacement pb = find_by_handle(p, n, &b);

    assert(pb.x == 0 && pb.width == 400);
    assert(pa.x == 400 && pa.width == 400);

    dump("smart left", p, n);
    trimming_destroy(t);
}

static void test_manual_split_equal_widths(void) {
    struct TrimmingConfig cfg = {0};
    cfg.manual_split = true;
    cfg.split_ratio = 0.5f;
    struct TrimmingTree *t = new_tree(&cfg);

    int a = 1, b = 2, c = 3;
    trimming_insert(t, &a, NULL, 0, 0, 0, 0, 0, 0);
    trimming_insert(t, &b, &a, 500, 300, 0, 0, 900, 600);
    trimming_insert(t, &c, &b, 600, 300, 450, 0, 450, 600);

    struct TrimmingLayoutParams lp = {
        .x = 0,
        .y = 0,
        .width = 900,
        .height = 600,
    };

    struct TrimmingPlacement p[4];
    size_t n = trimming_layout(t, &lp, p, 4);
    assert(n == 3);

    struct TrimmingPlacement pa = find_by_handle(p, n, &a);
    struct TrimmingPlacement pb = find_by_handle(p, n, &b);
    struct TrimmingPlacement pc = find_by_handle(p, n, &c);

    dump("manual split", p, n);

    // With manual_split on, all three should be roughly 1/3 wide
    assert(pa.width >= 250 && pa.width <= 350);
    assert(pb.width >= 250 && pb.width <= 350);
    assert(pc.width >= 250 && pc.width <= 350);

    trimming_destroy(t);
}

int main(void) {
    test_single_window();
    test_outer_gap_single();
    test_outer_gap_two();
    test_three_windows_dwindle();
    test_split_second_leaf();
    test_remove_promotes_sibling();
    test_smart_split_top();
    test_smart_split_left();
    test_manual_split_equal_widths();
    return 0;
}
