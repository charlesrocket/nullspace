#include "animation.h"

#include "nullspace.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <unistd.h>

int64_t timespec_to_ns(const struct timespec *ts) {
    return (int64_t)ts->tv_sec * 1000000000LL + (int64_t)ts->tv_nsec;
}

double ease_out_cubic(double t) {
    const double f = t - 1.0;
    return f * f * f + 1.0;
}

double animation_progress(
    const struct WindowAnimation *a, const struct timespec *now
) {
    if (a->duration_ms <= 0) { return 1.0; }

    const int64_t elapsed_ns =
        timespec_to_ns(now) - timespec_to_ns(&a->start_time);
    const int64_t duration_ns = (int64_t)a->duration_ms * 1000000LL;

    if (elapsed_ns <= 0) { return 0.0; }
    if (elapsed_ns >= duration_ns) { return 1.0; }

    return (double)elapsed_ns / (double)duration_ns;
}

// We arm a timer and request the next
// manage sequence only when it fires.

static int64_t anim_frame_interval_ns(void) {
    long hz = ANIM_DEFAULT_HZ;

    const char *env = getenv("NSP_ANIM_HZ");
    if (env != NULL && env[0] != '\0') {
        char *end = NULL;
        long v = strtol(env, &end, 10);

        if (end != env && *end == '\0' && v >= ANIM_MIN_HZ
            && v <= ANIM_MAX_HZ) {
            hz = v;
        } else {
            fprintf(
                stderr, "NSP_ANIM_HZ=%s is invalid (range is %d-%d)\n", env,
                ANIM_MIN_HZ, ANIM_MAX_HZ
            );
        }
    }

    return 1000000000LL / hz;
}

void anim_timer_init(void) {
    wm.anim_frame_ns = anim_frame_interval_ns();
    wm.anim_timer_armed = false;
    wm.anim_timer_fd =
        timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

    if (wm.anim_timer_fd < 0) {
        perror("timerfd_create");
        fprintf(
            stderr, "falling back to unpaced animation (busy manage_dirty)\n"
        );
    }
}

void anim_timer_arm(
    struct river_window_manager_v1 *manager, const struct timespec *now
) {
    if (wm.anim_timer_armed) { return; }

    if (wm.anim_timer_fd < 0) {
        // No timer available, fire at will!
        river_window_manager_v1_manage_dirty(manager);
        return;
    }

    int64_t now_ns = timespec_to_ns(now);
    int64_t frame = wm.anim_frame_ns;

    // Next multiple of `frame` strictly after now.
    int64_t delay_ns = (now_ns / frame + 1) * frame - now_ns;

    // TODO
    if (delay_ns < 1000) { delay_ns = 1000; }

    struct itimerspec spec = {
        .it_value =
            {
                       .tv_sec = (time_t)(delay_ns / 1000000000LL),
                       .tv_nsec = (long)(delay_ns % 1000000000LL),
                       },
        // Re-armed by the next manage pass if any animations remain.
    };

    if (timerfd_settime(wm.anim_timer_fd, 0, &spec, NULL) < 0) {
        perror("timerfd_settime");
        river_window_manager_v1_manage_dirty(manager);
        return;
    }

    wm.anim_timer_armed = true;
}

void anim_timer_fire(struct river_window_manager_v1 *manager) {
    uint64_t expirations;

    // Lock
    ssize_t n = read(wm.anim_timer_fd, &expirations, sizeof(expirations));
    (void)n;

    wm.anim_timer_armed = false;

    // Exactly one manage sequence per frame interval.
    river_window_manager_v1_manage_dirty(manager);
}
