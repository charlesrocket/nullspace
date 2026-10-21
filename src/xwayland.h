#pragma once

#include <stdbool.h>

struct wl_display;

bool xwayland_init(struct wl_display *display);

void xwayland_finish(void);
void xwayland_primary_update(void);
