#include "trimming.h"

#include <math.h>
#include <stdlib.h>

struct Node {
    bool is_split;
    bool split_h;
    bool split_locked;
    bool custom_leaf_split_h;
    float ratio;

    int32_t container_x, container_y, container_w, container_h;

    struct Node *first;
    struct Node *second;
    struct Node *parent;

    void *client;
};

struct TrimmingTree {
    struct Node *root;
    struct TrimmingConfig config;
};

static int count_block_items(struct Node *node, bool split_h) {
    if (node == NULL) { return 0; }
    if (!node->is_split || node->split_h != split_h) { return 1; }
    return count_block_items(node->first, split_h)
         + count_block_items(node->second, split_h);
}

static struct Node *find_leaf(struct Node *node, void *client) {
    if (node == NULL) { return NULL; }
    if (!node->is_split) { return node->client == client ? node : NULL; }
    struct Node *r = find_leaf(node->first, client);
    return r ? r : find_leaf(node->second, client);
}

static struct Node *first_leaf(struct Node *node) {
    if (node == NULL) { return NULL; }
    while (node->is_split) { node = node->first; }
    return node;
}

static void free_node(struct Node *node) {
    if (node == NULL) { return; }
    free_node(node->first);
    free_node(node->second);
    free(node);
}

static size_t count_leaves(struct Node *node) {
    if (node == NULL) { return 0; }
    if (!node->is_split) { return 1; }
    return count_leaves(node->first) + count_leaves(node->second);
}

static int get_block_path_and_ratios(
    struct Node *target, bool split_h, struct Node ***path_out, float **p_out
) {
    int depth = 1;
    struct Node *curr = target->parent;
    while (curr != NULL && curr->split_h == split_h) {
        depth++;
        curr = curr->parent;
    }

    struct Node **path = calloc((size_t)depth, sizeof(*path));
    float *p = calloc((size_t)depth, sizeof(*p));
    if (path == NULL || p == NULL) {
        free(path);
        free(p);
        *path_out = NULL;
        *p_out = NULL;
        return 0;
    }

    int path_len = 0;
    path[path_len++] = target;
    curr = target->parent;
    while (curr != NULL && curr->split_h == split_h) {
        path[path_len++] = curr;
        curr = curr->parent;
    }

    p[path_len - 1] = 1.0f;
    for (int i = path_len - 1; i > 0; i--) {
        struct Node *S = path[i];
        struct Node *child = path[i - 1];
        if (S->first == child) {
            p[i - 1] = p[i] * S->ratio;
        } else {
            p[i - 1] = p[i] * (1.0f - S->ratio);
        }
    }

    *path_out = path;
    *p_out = p;

    return path_len;
}

struct TrimmingTree *trimming_create(void) {
    struct TrimmingTree *t = calloc(1, sizeof(struct TrimmingTree));
    if (t == NULL) { return NULL; }
    t->config.split_ratio = 0.5f;
    return t;
}

void trimming_destroy(struct TrimmingTree *t) {
    if (t == NULL) { return; }
    free_node(t->root);
    free(t);
}

void trimming_set_config(
    struct TrimmingTree *t, const struct TrimmingConfig *cfg
) {
    if (t == NULL || cfg == NULL) { return; }
    t->config = *cfg;
}

size_t trimming_leaf_count(const struct TrimmingTree *t) {
    if (t == NULL) { return 0; }
    return count_leaves(t->root);
}

static void insert_split(
    struct TrimmingTree *t, void *new_c, struct Node *target, float ratio,
    bool as_first, bool split_h, bool lock
) {
    if (t->config.manual_split) {
        struct Node **path = NULL;
        float *p = NULL;
        int path_len = get_block_path_and_ratios(target, split_h, &path, &p);

        if (path != NULL && p != NULL) {
            int n_old = 1;
            if (path_len > 1) {
                n_old = count_block_items(path[path_len - 1], split_h);
            }
            float N = (float)(n_old + 1);

            for (int i = path_len - 1; i > 0; i--) {
                struct Node *S = path[i];
                struct Node *child = path[i - 1];
                float p_S = p[i];
                float p_first = p_S * S->ratio;

                if (S->first == child) {
                    float p_first_new = p_first * (N - 1.0f) / N + 1.0f / N;
                    float p_S_new = p_S * (N - 1.0f) / N + 1.0f / N;
                    S->ratio = p_first_new / p_S_new;
                } else {
                    float p_first_new = p_first * (N - 1.0f) / N;
                    float p_S_new = p_S * (N - 1.0f) / N + 1.0f / N;
                    S->ratio = p_first_new / p_S_new;
                }

                if (S->ratio < 0.001f) { S->ratio = 0.001f; }
                if (S->ratio > 0.999f) { S->ratio = 0.999f; }
            }
        }

        free(path);
        free(p);
    }

    struct Node *new_leaf = calloc(1, sizeof(struct Node));
    struct Node *split = calloc(1, sizeof(struct Node));

    if (new_leaf == NULL || split == NULL) {
        free(new_leaf);
        free(split);
        return;
    }

    new_leaf->client = new_c;

    split->is_split = true;
    split->split_h = split_h;
    split->split_locked = lock;
    split->custom_leaf_split_h = target->custom_leaf_split_h;
    new_leaf->custom_leaf_split_h = target->custom_leaf_split_h;
    split->ratio = ratio;

    if (as_first) {
        split->first = new_leaf;
        split->second = target;
    } else {
        split->first = target;
        split->second = new_leaf;
    }

    split->parent = target->parent;
    target->parent = split;
    new_leaf->parent = split;

    if (split->parent == NULL) {
        t->root = split;
    } else if (split->parent->first == target) {
        split->parent->first = split;
    } else {
        split->parent->second = split;
    }
}

void trimming_insert(
    struct TrimmingTree *t, void *new_c, void *focused, int32_t cursor_x,
    int32_t cursor_y, int32_t focus_x, int32_t focus_y, int32_t focus_w,
    int32_t focus_h
) {
    if (t == NULL || new_c == NULL) { return; }

    if (t->root == NULL) {
        struct Node *leaf = calloc(1, sizeof(struct Node));
        if (leaf == NULL) { return; }
        leaf->client = new_c;
        leaf->custom_leaf_split_h = true;
        t->root = leaf;

        return;
    }

    bool as_first = false;
    bool split_h = true;
    bool lock = false;
    float ratio = t->config.split_ratio;

    double fcx = (double)focus_x + (double)focus_w * 0.5;
    double fcy = (double)focus_y + (double)focus_h * 0.5;
    bool have_geom = (focus_w > 0 && focus_h > 0);

    if (t->config.smart_split && have_geom) {
        double nx = ((double)cursor_x - fcx) / ((double)focus_w * 0.5);
        double ny = ((double)cursor_y - fcy) / ((double)focus_h * 0.5);

        if (fabs(ny) > fabs(nx)) {
            split_h = false;
            as_first = (ny < 0);
        } else {
            split_h = true;
            as_first = (nx < 0);
        }

        lock = true;
    } else if (have_geom) {
        bool likely_h = (focus_w >= focus_h);
        split_h = likely_h;

        if (likely_h) {
            if (t->config.hsplit == 0) {
                as_first = ((double)cursor_x < fcx);
            } else {
                as_first = (t->config.hsplit == 2);
            }
        } else {
            if (t->config.vsplit == 0) {
                as_first = ((double)cursor_y < fcy);
            } else {
                as_first = (t->config.vsplit == 2);
            }
        }
    } else {
        split_h = true;
        as_first = false;
    }

    struct Node *target = focused ? find_leaf(t->root, focused) : NULL;
    if (target == NULL) { target = first_leaf(t->root); }
    if (target == NULL) { return; }

    if (t->config.manual_split) {
        split_h = target->custom_leaf_split_h;
        lock = true;
        as_first = false;

        struct Node **path = NULL;
        float *p = NULL;
        int path_len = get_block_path_and_ratios(target, split_h, &path, &p);

        if (path != NULL && p != NULL) {
            int n_old = 1;
            if (path_len > 1) {
                n_old = count_block_items(path[path_len - 1], split_h);
            }
            float N = (float)(n_old + 1);

            float p_target_old = p[0];
            float p_split_new = p_target_old * (N - 1.0f) / N + 1.0f / N;

            if (as_first) {
                ratio = (1.0f / N) / p_split_new;
            } else {
                ratio = (p_target_old * (N - 1.0f) / N) / p_split_new;
            }

            if (ratio < 0.001f) { ratio = 0.001f; }
            if (ratio > 0.999f) { ratio = 0.999f; }
        }

        free(path);
        free(p);
    }

    insert_split(t, new_c, target, ratio, as_first, split_h, lock);
}

void trimming_remove(struct TrimmingTree *t, void *client) {
    if (t == NULL) { return; }
    struct Node *leaf = find_leaf(t->root, client);
    if (leaf == NULL) { return; }

    struct Node *parent = leaf->parent;

    if (parent == NULL) {
        free(leaf);
        t->root = NULL;
        return;
    }

    if (t->config.manual_split) {
        bool split_h = parent->split_h;

        int path_len = 1;
        struct Node *cnt = parent->parent;
        while (cnt != NULL && cnt->split_h == split_h) {
            path_len++;
            cnt = cnt->parent;
        }

        struct Node **path = calloc((size_t)path_len, sizeof(*path));
        float *p = calloc((size_t)path_len, sizeof(*p));
        if (path != NULL && p != NULL) {
            path_len = 0;
            path[path_len++] = parent;

            struct Node *curr = parent->parent;
            while (curr != NULL && curr->split_h == split_h) {
                path[path_len++] = curr;
                curr = curr->parent;
            }

            p[path_len - 1] = 1.0f;

            for (int i = path_len - 1; i > 0; i--) {
                struct Node *S = path[i];
                struct Node *child = path[i - 1];
                if (S->first == child) {
                    p[i - 1] = p[i] * S->ratio;
                } else {
                    p[i - 1] = p[i] * (1.0f - S->ratio);
                }
            }

            float p_del = p[0]
                        * (parent->first == leaf ? parent->ratio
                                                 : (1.0f - parent->ratio));
            if (p_del > 0.999f) { p_del = 0.999f; }

            for (int i = path_len - 1; i > 0; i--) {
                struct Node *S = path[i];
                struct Node *child = path[i - 1];

                float p_S = p[i];
                float p_first = p_S * S->ratio;
                float denom = p_S - p_del;

                if (denom < 0.0001f) { denom = 0.0001f; }

                if (S->first == child) {
                    S->ratio = (p_first - p_del) / denom;
                } else {
                    S->ratio = p_first / denom;
                }

                if (S->ratio < 0.001f) { S->ratio = 0.001f; }
                if (S->ratio > 0.999f) { S->ratio = 0.999f; }
            }
        }

        free(path);
        free(p);
    }

    struct Node *sibling =
        (parent->first == leaf) ? parent->second : parent->first;
    struct Node *grandparent = parent->parent;

    sibling->parent = grandparent;

    if (!sibling->is_split
        || (!t->config.preserve_split && !t->config.smart_split)) {
        sibling->container_w = 0;
        sibling->container_h = 0;
    }

    if (grandparent == NULL) {
        t->root = sibling;
    } else if (grandparent->first == parent) {
        grandparent->first = sibling;
    } else {
        grandparent->second = sibling;
    }

    free(leaf);
    free(parent);
}

static void assign_node(
    struct Node *node, int32_t ax, int32_t ay, int32_t aw, int32_t ah,
    int32_t gap_h, int32_t gap_v, struct TrimmingPlacement *out, size_t *idx,
    size_t cap
) {
    if (node == NULL || *idx >= cap) { return; }

    if (!node->is_split) {
        if (node->client != NULL) {
            out[*idx].handle = node->client;
            out[*idx].x = ax;
            out[*idx].y = ay;
            out[*idx].width = aw > 1 ? aw : 1;
            out[*idx].height = ah > 1 ? ah : 1;
            (*idx)++;
        }
        return;
    }

    if (!node->split_locked && node->container_w == 0
        && node->container_h == 0) {
        node->split_h = (aw >= ah);
    }

    node->container_x = ax;
    node->container_y = ay;
    node->container_w = aw;
    node->container_h = ah;

    if (node->split_h) {
        int32_t w1 = (int32_t)((float)aw * node->ratio) - gap_h / 2;
        if (w1 < 1) { w1 = 1; }

        assign_node(node->first, ax, ay, w1, ah, gap_h, gap_v, out, idx, cap);
        assign_node(
            node->second, ax + w1 + gap_h, ay, aw - w1 - gap_h, ah, gap_h,
            gap_v, out, idx, cap
        );
    } else {
        int32_t h1 = (int32_t)((float)ah * node->ratio) - gap_v / 2;
        if (h1 < 1) { h1 = 1; }

        assign_node(node->first, ax, ay, aw, h1, gap_h, gap_v, out, idx, cap);
        assign_node(
            node->second, ax, ay + h1 + gap_v, aw, ah - h1 - gap_v, gap_h,
            gap_v, out, idx, cap
        );
    }
}

size_t trimming_assign(
    struct TrimmingTree *t, int32_t x, int32_t y, int32_t width, int32_t height,
    int32_t gap_inner_h, int32_t gap_inner_v, struct TrimmingPlacement *out,
    size_t cap
) {
    if (t == NULL || out == NULL) { return 0; }

    size_t idx = 0;
    assign_node(
        t->root, x, y, width, height, gap_inner_h, gap_inner_v, out, &idx, cap
    );

    return idx;
}

size_t trimming_layout(
    struct TrimmingTree *t, const struct TrimmingLayoutParams *p,
    struct TrimmingPlacement *out, size_t cap
) {
    if (t == NULL || p == NULL || out == NULL) { return 0; }

    int32_t g_oh = p->gap_outer_h > 0 ? p->gap_outer_h : 0;
    int32_t g_ov = p->gap_outer_v > 0 ? p->gap_outer_v : 0;
    int32_t g_ih = p->gap_inner_h > 0 ? p->gap_inner_h : 0;
    int32_t g_iv = p->gap_inner_v > 0 ? p->gap_inner_v : 0;

    int32_t x = p->x + g_oh;
    int32_t y = p->y + g_ov;
    int32_t w = p->width - 2 * g_oh;
    int32_t h = p->height - 2 * g_ov;

    if (w < 1) { w = 1; }
    if (h < 1) { h = 1; }

    return trimming_assign(t, x, y, w, h, g_ih, g_iv, out, cap);
}
