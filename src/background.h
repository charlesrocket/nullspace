#ifndef BACKGROUND_H
#define BACKGROUND_H

#include <stdbool.h>
#include <wayland-server-core.h>

bool background_effect_init(struct wl_display *display);
void background_effect_finish(void);

#endif // BACKGROUND_H
