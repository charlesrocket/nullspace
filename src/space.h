#pragma once

#include <stdbool.h>
#include <wayland-server-core.h>

struct Output;
struct wl_display;

bool ext_workspace_init(struct wl_display *display);
void ext_workspace_finish(void);

void ext_workspace_output_enter(struct Output *out);
void ext_workspace_output_leave(struct Output *out);

void ext_workspace_update_states(void);
