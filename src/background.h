#pragma once

#include <scenefx/types/wlr_scene.h>
#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>

bool background_effect_init(struct wl_display* display);
void background_effect_finish(void);

bool background_effect_surface_registered(struct wlr_surface* surface);
bool background_effect_surface_active(struct wlr_surface* surface);
