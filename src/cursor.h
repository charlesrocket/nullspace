#pragma once

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
    struct Window* window, enum SeatOp op, uint32_t edges
);

void cursor_handle_motion(struct wl_listener* l, void* data);
void cursor_handle_motion_absolute(struct wl_listener* l, void* data);
void cursor_handle_button(struct wl_listener* l, void* data);
void cursor_handle_axis(struct wl_listener* l, void* data);
void cursor_handle_frame(struct wl_listener* l, void* data);
void cursor_handle_swipe_begin(struct wl_listener* l, void* data);
void cursor_handle_swipe_update(struct wl_listener* l, void* data);
void cursor_handle_swipe_end(struct wl_listener* l, void* data);
void cursor_handle_pinch_begin(struct wl_listener* l, void* data);
void cursor_handle_pinch_update(struct wl_listener* l, void* data);
void cursor_handle_pinch_end(struct wl_listener* l, void* data);
void cursor_handle_hold_begin(struct wl_listener* l, void* data);
void cursor_handle_hold_end(struct wl_listener* l, void* data);

void cursor_window_destroyed(struct Window* window);
void cursor_refresh_focus(void);
void cursor_cancel_drag_state(void);
void cursor_end_interactive(void);
void cursor_end_interactive_for(struct Window* window);
void cursor_hide_init(struct wl_event_loop* loop);
void cursor_hide_finish(void);
void cursor_hide_now(void);

bool cursor_is_hidden(void);
bool cursor_is_interactive(void);

bool cursor_constraints_init(void);
void cursor_update_active_constraint(void);
void cursor_constraints_finish(void);
