#ifndef ANIMATION_H
#define ANIMATION_H

#include "config.h"
#include "nullspace.h"

#include <stdint.h>
#include <time.h>

#define ANIM_DURATION_SPACE CFG_ANIM_DURATION_SPACE
#define ANIM_DURATION_OPEN  CFG_ANIM_DURATION_OPEN
#define ANIM_DURATION_CLOSE CFG_ANIM_DURATION_CLOSE
#define ANIM_DURATION_TILE  CFG_ANIM_DURATION_TILE

#define ANIM_DEFAULT_HZ     CFG_ANIM_DEFAULT_HZ
#define ANIM_MIN_HZ         CFG_ANIM_MIN_HZ
#define ANIM_MAX_HZ         CFG_ANIM_MAX_HZ

// `window->x/y` the last position given to the compositor
// `window->prop_w/prop_h` the last proposed size

int64_t timespec_to_ns(const struct timespec *ts);

double ease_out_cubic(double t);

double
animation_progress(const struct WindowAnimation *a, const struct timespec *now);

void anim_timer_init(void);

void anim_timer_arm(
    struct river_window_manager_v1 *manager, const struct timespec *now
);

void anim_timer_fire(struct river_window_manager_v1 *manager);

#endif // ANIMATION_H
