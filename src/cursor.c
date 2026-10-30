#include "cursor.h"

#include "actions.h"
#include "animation.h"
#include "config.h"
#include "input.h"
#include "layouts/layout.h"
#include "manager.h"
#include "seat.h"
#include "server.h"
#include "session.h"
#include "window.h"

#include <dev/evdev/input-event-codes.h>
#include <nullspace/layouts.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/edges.h>

#define LOG_TOPIC "cursor"

struct InteractiveOp {
    struct Window *window;
    enum SeatOp op;

    uint32_t edges;
    uint32_t trigger_button;

    double cursor_start_x;
    double cursor_start_y;

    int32_t window_start_x;
    int32_t window_start_y;
    int32_t window_start_w;
    int32_t window_start_h;

    bool active;
};

static struct InteractiveOp interactive;
static struct Window *hovered_window;
static unsigned buttons_down;

static void interactive_apply(double cx, double cy) {
    struct InteractiveOp *g = &interactive;
    if (!g->active || g->window == NULL || g->window->closed) return;

    struct Window *v = g->window;

    int32_t dcx = (int32_t)(cx - g->cursor_start_x);
    int32_t dcy = (int32_t)(cy - g->cursor_start_y);

    if (g->op == SEAT_OP_MOVE) {
        int32_t nx = g->window_start_x + dcx;
        int32_t ny = g->window_start_y + dcy;
        window_set_position(v, nx, ny);

        v->saved_x = nx;
        v->saved_y = ny;
        return;
    }

    if (g->op == SEAT_OP_RESIZE) {
        int32_t nx = g->window_start_x;
        int32_t ny = g->window_start_y;
        int32_t nw = g->window_start_w;
        int32_t nh = g->window_start_h;

        if (g->edges & WLR_EDGE_RIGHT) nw += dcx;
        if (g->edges & WLR_EDGE_BOTTOM) nh += dcy;
        if (g->edges & WLR_EDGE_LEFT) {
            nw -= dcx;
            nx += dcx;
        }

        if (g->edges & WLR_EDGE_TOP) {
            nh -= dcy;
            ny += dcy;
        }

        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;

        window_set_size(v, nw, nh);
        window_set_position(v, nx, ny);
        v->saved_x = nx;
        v->saved_y = ny;
    }
}

void cursor_begin_interactive(
    struct Window *window, enum SeatOp op, uint32_t edges
) {
    if (server.locked || interactive.active) return;
    if (window == NULL || window->closed) return;

    interactive.window = window;
    interactive.op = op;
    interactive.edges = edges;
    interactive.trigger_button = 0;

    interactive.cursor_start_x = server.cursor->x;
    interactive.cursor_start_y = server.cursor->y;

    interactive.window_start_x = window->x;
    interactive.window_start_y = window->y;
    interactive.window_start_w = window->width > 0 ? window->width : 100;
    interactive.window_start_h = window->height > 0 ? window->height : 100;

    interactive.active = true;

    window_reparent(window, true);
    window_raise(window);

    wlr_seat_pointer_clear_focus(server.seat);
}

void cursor_end_interactive(void) {
    if (!interactive.active) return;

    interactive.active = false;
    interactive.window = NULL;
    interactive.op = SEAT_OP_NONE;
    interactive.edges = 0;
}

void cursor_end_interactive_for(struct Window *window) {
    if (!interactive.active) return;
    if (interactive.window == window) cursor_end_interactive();
}

// Handles window's mid-drag destroy.
void cursor_window_destroyed(struct Window *window) {
    if (hovered_window == window) hovered_window = NULL;
    cursor_end_interactive_for(window);
}

bool cursor_is_interactive(void) { return interactive.active; }

static void focus_window(struct Window *w) {
    if (w == NULL || w->closed || w->space_hidden || !w->mapped) return;
    if (server.locked) return;
    seat_focus(w); // no-ops if `w` already holds keyboard focus
}

static void focus_follow_on_motion(struct Window *under) {
    struct Window *prev = hovered_window;
    hovered_window = under; // always track, even when not activating

    if (!config.focus_follow || under == NULL) return;
    if (server.locked || !layouts_is_tiled(wm.layout)) return;
    if (wm.layout == LAYOUT_HORIZONTAL_MONOCLE) return;
    if (buttons_down > 0) return; // never switch focus mid-drag

    bool crossed = under != prev;
    bool no_keyboard_focus =
        server.seat->keyboard_state.focused_surface == NULL;

    if (!crossed && !no_keyboard_focus) return;

    focus_window(under);
}

static void cursor_manage_focus(bool motion) {
    struct wlr_surface *surface = NULL;
    double sx = 0, sy = 0;

    struct Window *under =
        window_at(server.cursor->x, server.cursor->y, &surface, &sx, &sy);

    if (motion) focus_follow_on_motion(surface != NULL ? under : NULL);

    if (surface == NULL) {
        wlr_seat_pointer_clear_focus(server.seat);
        wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");
        return;
    }

    wlr_seat_pointer_notify_enter(server.seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(
        server.seat, (uint32_t)animation_now_ms(), sx, sy
    );
}

static void cursor_update_focus(void) { cursor_manage_focus(false); }

void cursor_refresh_focus(void) {
    if (!interactive.active) cursor_update_focus();
}

static uint32_t current_keyboard_modifiers(void) {
    uint32_t mods = 0;

    struct Keyboard *kb;
    wl_list_for_each(kb, &server.keyboards, link) {
        mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
        if (mods != 0) break;
    }

    mods &= ~(uint32_t)WLR_MODIFIER_CAPS;
    mods &= ~(uint32_t)WLR_MODIFIER_MOD2;

    return mods;
}

void cursor_handle_motion(struct wl_listener *l, void *data) {
    (void)l;

    server_notify_activity();
    struct wlr_pointer_motion_event *ev = data;

    wlr_cursor_move(
        server.cursor, &ev->pointer->base, ev->delta_x, ev->delta_y
    );

    if (interactive.active) {
        interactive_apply(server.cursor->x, server.cursor->y);
        return;
    }

    cursor_manage_focus(true);
}

void cursor_handle_motion_absolute(struct wl_listener *l, void *data) {
    (void)l;

    server_notify_activity();
    struct wlr_pointer_motion_absolute_event *ev = data;

    wlr_cursor_warp_absolute(server.cursor, &ev->pointer->base, ev->x, ev->y);

    if (interactive.active) {
        interactive_apply(server.cursor->x, server.cursor->y);
        return;
    }

    cursor_manage_focus(true);
}

void cursor_handle_button(struct wl_listener *l, void *data) {
    (void)l;

    server_notify_activity();
    struct wlr_pointer_button_event *ev = data;

    if (interactive.active) {
        if (ev->state == WL_POINTER_BUTTON_STATE_RELEASED) {
            if (interactive.trigger_button == 0
                || ev->button == interactive.trigger_button) {
                cursor_end_interactive();
                buttons_down = 0;
                // Re-acquire focus at the current cursor position.
                cursor_update_focus();
            }
        }

        return;
    }

    uint32_t mods = current_keyboard_modifiers();

    if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED && !server.locked
        && !layouts_is_tiled(wm.layout)) {
        for (size_t i = 0; i < cfg_pointer_binds_len; i++) {
            const struct PointerDef *pb = &cfg_pointer_binds[i];

            if (pb->button != ev->button) continue;
            if (pb->mods != mods) continue;

            struct wlr_surface *surf = NULL;
            double sx = 0, sy = 0;
            struct Window *under =
                window_at(server.cursor->x, server.cursor->y, &surf, &sx, &sy);
            if (under == NULL || under->closed || under->space_hidden) break;

            focus_window(under);

            if (pb->action == ACTION_MOVE) {
                cursor_begin_interactive(under, SEAT_OP_MOVE, 0);
                interactive.trigger_button = ev->button;
            } else if (pb->action == ACTION_RESIZE) {
                cursor_begin_interactive(
                    under, SEAT_OP_RESIZE, WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT
                );

                interactive.trigger_button = ev->button;
            }

            return;
        }
    }

    if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED && !server.locked) {
        struct wlr_surface *surf = NULL;
        double sx = 0, sy = 0;
        struct Window *under =
            window_at(server.cursor->x, server.cursor->y, &surf, &sx, &sy);

        focus_window(under);
    }

    wlr_seat_pointer_notify_button(
        server.seat, ev->time_msec, ev->button, ev->state
    );

    if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        buttons_down++;
        session_lock_click_focus();
    } else if (buttons_down > 0) {
        buttons_down--;
    }

    cursor_update_focus();
}

void cursor_handle_axis(struct wl_listener *l, void *data) {
    (void)l;

    server_notify_activity();
    if (interactive.active) return;

    struct wlr_pointer_axis_event *ev = data;

    wlr_seat_pointer_notify_axis(
        server.seat, ev->time_msec, ev->orientation, ev->delta,
        ev->delta_discrete, ev->source, ev->relative_direction
    );
}

void cursor_handle_frame(struct wl_listener *l, void *data) {
    (void)l;
    (void)data;

    wlr_seat_pointer_notify_frame(server.seat);
}
