#pragma once

#include <wayland-server-core.h>

struct wl_display;
struct wl_global;

struct Controller {
    struct wl_global* global;
};

struct Controller* controller_create(struct wl_display* display);

struct Window;
struct wl_display;

void controller_init(struct wl_display* display);
void controller_destroy(struct Controller* c);
void controller_notify_spaces(void);
void controller_notify_layout(void);
void controller_notify_kb_layout(void);
void controller_notify_window_opened(struct Window* w);
void controller_notify_window_closed(struct Window* w);
void controller_notify_window_meta(struct Window* w);
void controller_notify_focus(void);
