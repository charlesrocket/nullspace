#ifndef ANIMATION_H
#define ANIMATION_H

#include <stdbool.h>
#include <stdint.h>

enum AnimationKind {
    ANIM_NONE,
    ANIM_MOVE,
    ANIM_OPEN,
    ANIM_CLOSE,
    ANIM_SPACE,
    ANIM_KIND_COUNT,
};

struct AnimationBox {
    int32_t x, y;
    int32_t width, height;
};

struct Animation {
    enum AnimationKind kind;
    struct AnimationBox from;
    struct AnimationBox to;
    struct AnimationBox current;
    int64_t start_ms;
    int32_t duration_ms;
    bool running;
};

struct AnimationsConfig {
    int32_t duration_move;
    int32_t duration_open;
    int32_t duration_close;
    int32_t duration_space;

    bool enabled;
    bool open_from_top;
};

void animation_init(void);

double animation_ease(double t, enum AnimationKind kind);

int64_t animation_now_ms(void);

void animation_start(
    struct Animation *a, enum AnimationKind kind,
    const struct AnimationBox *from, const struct AnimationBox *to,
    int32_t duration_ms
);

bool animation_step(struct Animation *a, int64_t now_ms);

static inline bool
animation_box_eq(const struct AnimationBox *a, const struct AnimationBox *b) {
    return a->x == b->x && a->y == b->y && a->width == b->width
        && a->height == b->height;
}

static inline struct AnimationBox
animation_box(int32_t x, int32_t y, int32_t w, int32_t h) {
    struct AnimationBox b = {x, y, w, h};
    return b;
}

#endif // ANIMATION_H
