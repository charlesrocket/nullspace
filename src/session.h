#pragma once

#include <stdbool.h>

struct wl_display;
struct Output;

bool session_lock_init(struct wl_display *display);

void session_lock_detach_listeners(void);
void session_lock_update_geometry(void);
void session_lock_output_committed(struct Output *out);
void session_lock_click_focus(void);
void session_lock_outputs_changed(void);
