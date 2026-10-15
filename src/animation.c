#include "animation.h"

#include "config.h"

#include <stdint.h>
#include <sys/_clock_id.h>
#include <time.h>

#define ANIM_BAKED_POINTS 64

struct AnimCurve {
    double x1, y1, x2, y2;
};

struct BakedCurve {
    double x[ANIM_BAKED_POINTS];
    double y[ANIM_BAKED_POINTS];
};

static struct BakedCurve baked_none;
static struct BakedCurve baked_move;
static struct BakedCurve baked_open;
static struct BakedCurve baked_close;
static struct BakedCurve baked_space;

static const struct BakedCurve *baked_for(enum AnimationType type) {
    switch (type) {
        case ANIM_NONE: return &baked_none;
        case ANIM_MOVE: return &baked_move;
        case ANIM_OPEN: return &baked_open;
        case ANIM_CLOSE: return &baked_close;
        case ANIM_SPACE: return &baked_space;
    }

    return &baked_move; // invalid value
}

static double bezier_component(double t, double p1, double p2) {
    double mt = 1.0 - t;
    return 3.0 * mt * mt * t * p1 + 3.0 * mt * t * t * p2 + t * t * t;
}

static void bake(struct BakedCurve *b, const struct AnimCurve *c) {
    for (int i = 0; i < ANIM_BAKED_POINTS; i++) {
        double t = (double)i / (double)(ANIM_BAKED_POINTS - 1);
        b->x[i] = bezier_component(t, c->x1, c->x2);
        b->y[i] = bezier_component(t, c->y1, c->y2);
    }
}

void animation_init(void) {
    bake(&baked_none, &(struct AnimCurve){0.25, 0.10, 0.25, 1.00});
    bake(&baked_move, &(struct AnimCurve)CFG_ANIM_CURVE_MOVE);
    bake(&baked_open, &(struct AnimCurve)CFG_ANIM_CURVE_OPEN);
    bake(&baked_close, &(struct AnimCurve)CFG_ANIM_CURVE_CLOSE);
    bake(&baked_space, &(struct AnimCurve)CFG_ANIM_CURVE_SPACE);
}

double animation_ease(double t, enum AnimationType type) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;

    const struct BakedCurve *c = baked_for(type);

    // Binary search the segment that brackets the requested x (= t) and
    // linearly interpolate y.
    int lo = 0, hi = ANIM_BAKED_POINTS - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (c->x[mid] <= t)
            lo = mid;
        else
            hi = mid;
    }

    double x0 = c->x[lo], x1 = c->x[hi];
    double y0 = c->y[lo], y1 = c->y[hi];
    if (x1 - x0 < 1e-9) return y1;

    double f = (t - x0) / (x1 - x0);
    return y0 + (y1 - y0) * f;
}

int64_t animation_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

void animation_start(
    struct Animation *a, enum AnimationType type,
    const struct AnimationBox *from, const struct AnimationBox *to,
    int32_t duration_ms
) {
    a->type = type;
    a->from = *from;
    a->to = *to;
    a->current = *from;
    a->duration_ms = duration_ms;
    a->start_ms = animation_now_ms();
    a->running = duration_ms > 0 && !animation_box_eq(from, to);

    if (!a->running) a->current = *to;
}

bool animation_step(struct Animation *a, int64_t now_ms) {
    if (!a->running) return false;
    if (a->duration_ms <= 0) {
        a->current = a->to;
        a->running = false;
        return false;
    }

    double t = (double)(now_ms - a->start_ms) / (double)a->duration_ms;
    if (t >= 1.0) {
        a->current = a->to;
        a->running = false;
        return false;
    }
    if (t < 0.0) t = 0.0;

    double e = animation_ease(t, a->type);
    a->current.x = a->from.x + (int32_t)((double)(a->to.x - a->from.x) * e);
    a->current.y = a->from.y + (int32_t)((double)(a->to.y - a->from.y) * e);

    a->current.width =
        a->from.width + (int32_t)((double)(a->to.width - a->from.width) * e);

    a->current.height =
        a->from.height + (int32_t)((double)(a->to.height - a->from.height) * e);

    return true;
}
