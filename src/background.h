#pragma once

#include <stdbool.h>
#include <wayland-server-core.h>

bool background_effect_init(struct wl_display *display);
void background_effect_finish(void);
