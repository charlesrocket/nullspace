#ifndef ANIMATION_H
#define ANIMATION_H

#include "nullspace.h"

#include <stdint.h>
#include <time.h>

// Animations:
// `window->x/y` is the last position given to the compositor, and
// `window->prop_w/prop_h` is the last proposed size. `window_set_position()`
// and `window_send_size()` are the only writers, and new animations start
// from these values.

#define ANIM_DURATION_OPEN  200 // ms, grow-in
#define ANIM_DURATION_CLOSE 200 // ms, shrink-out
#define ANIM_DURATION_TILE  200 // ms, tiled layout transitions

// Animation tick rate. Override with the `NSP_ANIM_HZ` environment variable.
#define ANIM_DEFAULT_HZ     80
#define ANIM_MIN_HZ         60
#define ANIM_MAX_HZ         120

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
