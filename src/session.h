#pragma once

#include <stdbool.h>

struct wl_display;
struct Output;
struct wlr_session_lock_manager_v1;
struct wlr_session_lock_manager_v1* session_lock_get_manager(void);

bool session_lock_init(struct wl_display* display);

void session_lock_detach_listeners(void);
void session_lock_update_geometry(void);
void session_lock_output_committed(struct Output* out);
void session_lock_click_focus(void);
void session_lock_outputs_changed(void);
