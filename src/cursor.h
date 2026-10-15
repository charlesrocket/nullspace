#ifndef CURSOR_H
#define CURSOR_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct Window;

enum SeatOp {
    SEAT_OP_NONE,
    SEAT_OP_MOVE,
    SEAT_OP_RESIZE,
};

void cursor_begin_interactive(
    struct Window *view, enum SeatOp op, uint32_t edges
);

void cursor_end_interactive(void);
void cursor_end_interactive_for(struct Window *view);
void cursor_refresh_focus(void);
bool cursor_is_interactive(void);

void cursor_handle_motion(struct wl_listener *l, void *data);
void cursor_handle_motion_absolute(struct wl_listener *l, void *data);
void cursor_handle_button(struct wl_listener *l, void *data);
void cursor_handle_axis(struct wl_listener *l, void *data);
void cursor_handle_frame(struct wl_listener *l, void *data);

#endif // CURSOR_H
