#include "cursor.h"

#include "actions.h"
#include "animation.h"
#include "config.h"
#include "decoration.h"
#include "input.h"
#include "layout/layout.h"
#include "manager.h"
#include "output.h"
#include "overview.h"
#include "seat.h"
#include "server.h"
#include "session.h"
#include "window.h"

#include <dev/evdev/input-event-codes.h>
#include <math.h>
#include <nullspace/layouts.h>
#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/_clock_id.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/util/edges.h>
#include <wlr/util/region.h>

#define LOG_TOPIC           "cursor"
#define EDGE_GRAB_PX        6
#define SPACE_SWIPE_FINGERS 4u
#define SPACE_SWIPE_STEP    200.0
#define SPACE_SCROLL_STEP   40.0

struct InteractiveOp {
    struct Window* window;
    enum SeatOp op;

    double cursor_start_x, cursor_start_y;
    uint32_t edges, trigger_button;
    int32_t window_start_x, window_start_y, window_start_w, window_start_h;
    bool active;
};

struct PointerConstraintTracker {
    struct wl_list link;
    struct wlr_pointer_constraint_v1* constraint;
    struct wl_listener set_region, destroy;
};

static struct wl_list constraint_trackers;
static struct wlr_pointer_constraint_v1* active_constraint;
static pixman_region32_t confine_region;
static bool active_confine_requires_warp, constraints_initialized;
static struct wl_listener active_constraint_commit, new_constraint_listener;

static struct InteractiveOp interactive;
static struct Window* hovered_window;
static unsigned buttons_down;

static struct Window* pointer_grab_window;
static double pointer_grab_lx, pointer_grab_ly; // global cursor at press
static double pointer_grab_sx, pointer_grab_sy; // surface at press
static uint64_t last_activity_ms;
static struct wl_event_source* hide_timer;
static bool hide_timer_armed, hide_idle_on, hide_typing_on, cursor_hidden,
    edge_cursor_active;

static bool swipe_forwarded; // goes to the client
static bool swipe_space;     // drives the space switching
static bool space_scroll_overview;
static double swipe_accum, space_scroll_accum, space_scroll_h_accum;

static int32_t overview_touch_id = -1; // first finger owns the overview drag

bool cursor_is_hidden(void) { return cursor_hidden; }

void cursor_overview_touch_reset(void) { overview_touch_id = -1; }

static void set_xcursor(const char* name) {
    if (cursor_hidden) return;
    wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, name);
}

static void drag_icon_update_position(void) {
    if (server.seat == NULL || server.seat->drag == NULL) return;

    struct wlr_drag_icon* icon = server.seat->drag->icon;
    if (icon == NULL || icon->data == NULL) return;

    struct wlr_scene_tree* tree = icon->data;
    wlr_scene_node_set_position(
        &tree->node, (int32_t)round(server.cursor->x),
        (int32_t)round(server.cursor->y)
    );
}

static void interactive_apply(double cx, double cy) {
    struct InteractiveOp* g = &interactive;
    if (!g->active || g->window == NULL || g->window->closed) return;

    struct Window* v = g->window;

    int32_t dcx = (int32_t)(cx - g->cursor_start_x);
    int32_t dcy = (int32_t)(cy - g->cursor_start_y);

    if (g->op == SEAT_OP_MOVE) {
        int32_t nx = g->window_start_x + dcx;
        int32_t ny = g->window_start_y + dcy;

        struct wlr_box ua;
        if (window_usable_area(v, &ua)) {
            const int32_t deco_top = window_visual_top_offset(v);

            int32_t w = v->width > 0 ? v->width : 1;
            int32_t h = v->height > 0 ? v->height : 1;

            if (nx + w > ua.x + ua.width) nx = ua.x + ua.width - w;
            if (ny + h > ua.y + ua.height) ny = ua.y + ua.height - h;
            if (nx < ua.x) nx = ua.x;
            if (ny < ua.y + deco_top) ny = ua.y + deco_top;
        }

        window_set_position(v, nx, ny);

        v->saved_x = nx;
        v->saved_y = ny;

        struct Window* child;
        wl_list_for_each(child, &wm.windows, link) {
            if (child->closed || !child->mapped || child->space_hidden)
                continue;
            if (child == v) continue;
            if (window_parent(child) != v) continue;

            int32_t cw = child->width > 0 ? child->width : 1;
            int32_t ch = child->height > 0 ? child->height : 1;
            int32_t vw = v->width > 0 ? v->width : 1;
            int32_t vh = v->height > 0 ? v->height : 1;

            window_set_position(child, nx + (vw - cw) / 2, ny + (vh - ch) / 2);
        }

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

        // Pin the stationary edge when hints overrule the drag.
        int32_t pre_w = nw, pre_h = nh;
        window_clamp_to_size_hints(v, &nw, &nh);
        if (nw != pre_w && (g->edges & WLR_EDGE_LEFT)) nx -= (nw - pre_w);
        if (nh != pre_h && (g->edges & WLR_EDGE_TOP)) ny -= (nh - pre_h);

        struct wlr_box ua;
        if (window_usable_area(v, &ua)) {
            const int32_t deco_top = window_visual_top_offset(v);
            int32_t max_h = ua.height - deco_top;
            if (max_h < 1) max_h = 1;

            if (nw > ua.width) nw = ua.width;
            if (nh > max_h) nh = max_h;

            const int32_t min_y = ua.y + deco_top;
            if (ny < min_y) {
                nh -= (min_y - ny);
                ny = min_y;
            }

            if (nx + nw > ua.x + ua.width) nw = ua.x + ua.width - nx;
            if (ny + nh > ua.y + ua.height) nh = ua.y + ua.height - ny;
            if (nx < ua.x) nx = ua.x;

            if (nw < 1) nw = 1;
            if (nh < 1) nh = 1;
        }

        window_set_size(v, nw, nh);
        window_set_position(v, nx, ny);
        v->saved_x = nx;
        v->saved_y = ny;
    }
}

static struct wlr_surface*
surface_at(double lx, double ly, double* sx, double* sy) {
    struct wlr_surface* surf = NULL;
    double ssx = 0, ssy = 0;
    window_at(lx, ly, &surf, &ssx, &ssy);

    if (surf == NULL) return NULL;
    if (sx) *sx = ssx;
    if (sy) *sy = ssy;

    return surf;
}

static void check_constraint_region(void) {
    if (active_constraint == NULL) return;

    if (active_confine_requires_warp) {
        active_confine_requires_warp = false;

        double sx, sy;
        if (surface_at(server.cursor->x, server.cursor->y, &sx, &sy)
                == active_constraint->surface
            && !pixman_region32_contains_point(
                &active_constraint->region, (int)floor(sx), (int)floor(sy), NULL
            )) {
            int nboxes;
            pixman_box32_t* boxes =
                pixman_region32_rectangles(&active_constraint->region, &nboxes);

            if (nboxes > 0) {
                double tx = (boxes[0].x1 + boxes[0].x2) / 2.0;
                double ty = (boxes[0].y1 + boxes[0].y2) / 2.0;
                double gx = server.cursor->x + (tx - sx);
                double gy = server.cursor->y + (ty - sy);

                wlr_cursor_warp_closest(server.cursor, NULL, gx, gy);
                cursor_refresh_focus();
            }
        }
    }

    if (active_constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
        pixman_region32_copy(&confine_region, &active_constraint->region);
    } else {
        pixman_region32_clear(&confine_region);
    }
}

static void handle_active_constraint_commit(struct wl_listener* l, void* data) {
    (void)l;
    (void)data;

    check_constraint_region();
}

static void handle_constraint_set_region(struct wl_listener* l, void* data) {
    (void)data;
    struct PointerConstraintTracker* t = wl_container_of(l, t, set_region);
    if (t->constraint == active_constraint) active_confine_requires_warp = true;
}

static void warp_to_constraint_cursor_hint(void) {
    if (active_constraint == NULL) return;
    if (!active_constraint->current.cursor_hint.enabled) return;

    double sx = active_constraint->current.cursor_hint.x;
    double sy = active_constraint->current.cursor_hint.y;

    struct Window* win = NULL;
    struct Window* w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->closed) continue;
        if (window_wlr_surface(w) == active_constraint->surface) {
            win = w;
            break;
        }
    }

    if (win == NULL || win->scene_tree == NULL) return;

    int wx = 0, wy = 0;
    if (!wlr_scene_node_coords(&win->scene_tree->node, &wx, &wy)) return;

    wlr_cursor_warp(server.cursor, NULL, wx + sx, wy + sy);
    wlr_seat_pointer_warp(server.seat, sx, sy);
}

static void handle_constraint_destroy(struct wl_listener* l, void* data) {
    (void)data;

    struct PointerConstraintTracker* t = wl_container_of(l, t, destroy);

    if (t->constraint == active_constraint) {
        warp_to_constraint_cursor_hint();
        wl_list_remove(&active_constraint_commit.link);
        wl_list_init(&active_constraint_commit.link);
        active_constraint = NULL;
        pixman_region32_clear(&confine_region);
    }

    wl_list_remove(&t->set_region.link);
    wl_list_remove(&t->destroy.link);
    wl_list_remove(&t->link);
    free(t);
}

void cursor_update_active_constraint(void) {
    if (!constraints_initialized) return;

    struct wlr_surface* focused = server.seat->pointer_state.focused_surface;
    struct wlr_pointer_constraint_v1* want = NULL;

    if (focused != NULL) {
        want = wlr_pointer_constraints_v1_constraint_for_surface(
            server.pointer_constraints, focused, server.seat
        );
    }

    if (want == active_constraint) return;

    if (active_constraint != NULL) {
        wlr_pointer_constraint_v1_send_deactivated(active_constraint);
        wl_list_remove(&active_constraint_commit.link);
        wl_list_init(&active_constraint_commit.link);
        active_constraint = NULL;
    }

    if (want == NULL) return;

    active_constraint = want;
    active_confine_requires_warp = true;

    if (pixman_region32_not_empty(&want->current.region)) {
        pixman_region32_intersect(
            &want->region, &want->surface->input_region, &want->current.region
        );
    } else {
        pixman_region32_copy(&want->region, &want->surface->input_region);
    }

    check_constraint_region();
    wlr_pointer_constraint_v1_send_activated(want);

    active_constraint_commit.notify = handle_active_constraint_commit;
    wl_signal_add(&want->surface->events.commit, &active_constraint_commit);
}

static void handle_new_constraint(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_pointer_constraint_v1* constraint = data;

    struct PointerConstraintTracker* t = calloc(1, sizeof(*t));
    if (t == NULL) return;
    t->constraint = constraint;

    t->set_region.notify = handle_constraint_set_region;
    wl_signal_add(&constraint->events.set_region, &t->set_region);

    t->destroy.notify = handle_constraint_destroy;
    wl_signal_add(&constraint->events.destroy, &t->destroy);

    wl_list_insert(&constraint_trackers, &t->link);

    struct wlr_surface* focused = server.seat->pointer_state.focused_surface;
    if (focused != NULL && focused == constraint->surface) {
        cursor_update_active_constraint();
    }
}

bool cursor_constraints_init(void) {
    if (constraints_initialized) return true;
    if (server.pointer_constraints == NULL) return false;

    wl_list_init(&constraint_trackers);
    pixman_region32_init(&confine_region);
    wl_list_init(&active_constraint_commit.link);

    new_constraint_listener.notify = handle_new_constraint;
    wl_signal_add(
        &server.pointer_constraints->events.new_constraint,
        &new_constraint_listener
    );

    constraints_initialized = true;
    return true;
}

void cursor_constraints_finish(void) {
    if (!constraints_initialized) return;

    wl_list_remove(&new_constraint_listener.link);

    struct PointerConstraintTracker *t, *tmp;
    wl_list_for_each_safe(t, tmp, &constraint_trackers, link) {
        wl_list_remove(&t->set_region.link);
        wl_list_remove(&t->destroy.link);
        wl_list_remove(&t->link);
        free(t);
    }

    if (active_constraint != NULL) {
        wl_list_remove(&active_constraint_commit.link);
        wl_list_init(&active_constraint_commit.link);
        active_constraint = NULL;
    }

    pixman_region32_fini(&confine_region);
    constraints_initialized = false;
}

void cursor_cancel_drag_state(void) {
    cursor_end_interactive();
    buttons_down = 0;
    pointer_grab_window = NULL;
}

void cursor_begin_interactive(
    struct Window* window, enum SeatOp op, uint32_t edges
) {
    if (server.locked || interactive.active) return;
    if (window == NULL || window->closed) return;
    if (window->maximized || window->fullscreen) return;

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

    if (op == SEAT_OP_RESIZE) {
        wm.resizing = true;
        if (window->decoration != NULL && window->decoration->shadow != NULL) {
            wlr_scene_node_set_enabled(
                &window->decoration->shadow->node, false
            );
        }
    }

    window_reparent(window, true);
    window_raise(window);

    wlr_seat_pointer_clear_focus(server.seat);
}

void cursor_end_interactive(void) {
    if (!interactive.active) return;

    struct Window* was = interactive.window;
    bool was_resize = (interactive.op == SEAT_OP_RESIZE);

    interactive.active = false;
    interactive.window = NULL;
    interactive.op = SEAT_OP_NONE;
    interactive.edges = 0;

    if (was_resize) {
        wm.resizing = false;
        if (was != NULL && !was->closed && was->decoration != NULL) {
            deco_update_position(was);
        }
    }

    wm_mark_layout_dirty();
}

void cursor_end_interactive_for(struct Window* window) {
    if (!interactive.active) return;
    if (interactive.window == window) cursor_end_interactive();
}

// Handles window's mid-drag destroy.
void cursor_window_destroyed(struct Window* window) {
    if (hovered_window == window) hovered_window = NULL;
    if (pointer_grab_window == window) pointer_grab_window = NULL;
    cursor_end_interactive_for(window);
}

bool cursor_is_interactive(void) { return interactive.active; }

static void focus_window(struct Window* w) {
    if (w == NULL || w->closed || w->space_hidden || !w->mapped) return;
    if (server.locked) return;
    seat_focus(w); // no-ops if `w` already holds keyboard focus
}

static void focus_follow_on_motion(struct Window* under) {
    struct Window* prev = hovered_window;
    hovered_window = under; // always track, even when not activating

    if (!config.focus_follow || under == NULL) return;
    if (server.locked || !layouts_is_tiled(wm.layout)) return;
    if (wm.layout == LAYOUT_HORIZONTAL_MONOCLE) return;
    if (buttons_down > 0) return; // never switch focus mid-drag
    if (wlr_seat_keyboard_has_grab(server.seat)) return;

    bool crossed = under != prev;
    bool no_keyboard_focus =
        server.seat->keyboard_state.focused_surface == NULL;

    if (!crossed && !no_keyboard_focus) return;

    focus_window(under);
}

static void send_grabbed_motion(void) {
    if (pointer_grab_window == NULL || pointer_grab_window->closed
        || pointer_grab_window->space_hidden || server.locked) {
        return;
    }

    if (window_wlr_surface(pointer_grab_window) == NULL) return;

    double dx = server.cursor->x - pointer_grab_lx;
    double dy = server.cursor->y - pointer_grab_ly;

    wlr_seat_pointer_notify_motion(
        server.seat, (uint32_t)animation_now_ms(), pointer_grab_sx + dx,
        pointer_grab_sy + dy
    );
}

static uint32_t edge_zone(struct Window* w, double cx, double cy) {
    if (w == NULL || w->closed || !w->mapped || w->space_hidden) return 0;
    if (w->fullscreen || layouts_is_tiled(wm.layout)) return 0;
    if (w->width <= 0 || w->height <= 0) return 0;

    int32_t x0 = w->x, y0 = w->y;
    int32_t x1 = x0 + w->width, y1 = y0 + w->height;

    if (cx < x0 || cx > x1) return 0;
    if (cy < y0 || cy > y1) return 0;

    uint32_t edges = 0;
    if (cx <= x0 + EDGE_GRAB_PX)
        edges |= WLR_EDGE_LEFT;
    else if (cx >= x1 - EDGE_GRAB_PX)
        edges |= WLR_EDGE_RIGHT;
    if (cy <= y0 + EDGE_GRAB_PX)
        edges |= WLR_EDGE_TOP;
    else if (cy >= y1 - EDGE_GRAB_PX)
        edges |= WLR_EDGE_BOTTOM;
    return edges;
}

static const char* xcursor_for_edges(uint32_t edges) {
    switch (edges) {
        case WLR_EDGE_LEFT | WLR_EDGE_TOP: return "nw-resize";
        case WLR_EDGE_RIGHT | WLR_EDGE_TOP: return "ne-resize";
        case WLR_EDGE_LEFT | WLR_EDGE_BOTTOM: return "sw-resize";
        case WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM: return "se-resize";
        case WLR_EDGE_LEFT: return "w-resize";
        case WLR_EDGE_RIGHT: return "e-resize";
        case WLR_EDGE_TOP: return "n-resize";
        case WLR_EDGE_BOTTOM: return "s-resize";
        default: return NULL;
    }
}

static void cursor_manage_focus(bool motion) {
    if (buttons_down > 0 && !wlr_seat_pointer_has_grab(server.seat)) {
        if (motion) { send_grabbed_motion(); }
        return;
    }

    struct wlr_surface* surface = NULL;
    double sx = 0, sy = 0;

    struct Window* under =
        window_at(server.cursor->x, server.cursor->y, &surface, &sx, &sy);

    if (motion) focus_follow_on_motion(surface != NULL ? under : NULL);

    if (surface == NULL) {
        wlr_seat_pointer_notify_clear_focus(server.seat);
        set_xcursor("default");
        edge_cursor_active = false;
        cursor_update_active_constraint();
        return;
    }

    uint32_t edges = edge_zone(under, server.cursor->x, server.cursor->y);
    const char* xc = xcursor_for_edges(edges);

    if (xc != NULL) {
        set_xcursor(xc);
        edge_cursor_active = true;
    } else if (edge_cursor_active) {
        edge_cursor_active = false;
        wlr_seat_pointer_notify_clear_focus(server.seat);
        set_xcursor("default");
    }

    wlr_seat_pointer_notify_enter(server.seat, surface, sx, sy);
    cursor_update_active_constraint();
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

    struct Keyboard* kb;
    wl_list_for_each(kb, &server.keyboards, link) {
        mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
        if (mods != 0) break;
    }

    mods &= ~(uint32_t)WLR_MODIFIER_CAPS;
    mods &= ~(uint32_t)WLR_MODIFIER_MOD2;

    return mods;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void arm_hide_timer(int ms) { // ms >= 1 (0 would disarm)
    hide_timer_armed = true;
    wl_event_source_timer_update(hide_timer, ms);
}

static void hide_cursor(void) {
    if (cursor_hidden) return;
    cursor_hidden = true;
    wlr_cursor_unset_image(server.cursor);
}

static void show_cursor(void) {
    cursor_hidden = false;
    wlr_seat_pointer_notify_clear_focus(server.seat);
    set_xcursor("default");
    cursor_refresh_focus();
}

static int hide_timer_cb(void* data) {
    (void)data;

    hide_timer_armed = false;

    if (cursor_hidden) return 0;
    if (buttons_down > 0 || interactive.active) return 0; // wake re-arms

    uint64_t idle = now_ms() - last_activity_ms;
    if (idle < CFG_CURSOR_HIDE_MS) {
        arm_hide_timer((int)(CFG_CURSOR_HIDE_MS - idle)); // sleep the remainder
        return 0;
    }

    hide_cursor();
    return 0;
}

void cursor_hide_init(struct wl_event_loop* loop) {
    hide_typing_on = CFG_CURSOR_HIDE_TYPING;
    hide_idle_on = CFG_CURSOR_HIDE_IDLE;

    if (!hide_idle_on || hide_timer != NULL) return;

    hide_timer = wl_event_loop_add_timer(loop, hide_timer_cb, NULL);
    if (hide_timer == NULL) {
        hide_idle_on = false;
        return;
    }

    last_activity_ms = now_ms();
    arm_hide_timer(CFG_CURSOR_HIDE_MS * 2);
}

void cursor_hide_finish(void) {
    if (hide_timer != NULL) {
        wl_event_source_remove(hide_timer);
        hide_timer = NULL;
    }

    hide_timer_armed = false;
    hide_idle_on = hide_typing_on = false;
    if (cursor_hidden) show_cursor();
}

void cursor_hide_now(void) {
    if (!hide_typing_on) return;
    if (buttons_down > 0 || interactive.active) return;
    hide_cursor();
}

static void cursor_wake(void) {
    if (hide_idle_on) {
        last_activity_ms = now_ms();
        if (!hide_timer_armed) arm_hide_timer(CFG_CURSOR_HIDE_MS);
    }

    if (cursor_hidden) show_cursor();
}

static void cursor_switch_space(int dir) {
    struct Output* out = output_focused();
    if (out == NULL) return;

    int next = out->current_space + dir;
    if (next < 0 || next >= SPACE_COUNT) return; // clamp
    wm_switch_space_on(out, next);
}

struct TouchPoint {
    double start_lx, start_ly, start_sx, start_sy;
    struct wlr_surface* surface; // ?NULL
    struct wl_listener surface_destroy;
    struct wl_list link;
    int32_t touch_id;
};

static struct wl_list touch_points = {&touch_points, &touch_points};

static struct TouchPoint* touch_point_find(int32_t touch_id) {
    struct TouchPoint* tp;
    wl_list_for_each(tp, &touch_points, link) {
        if (tp->touch_id == touch_id) return tp;
    }

    return NULL;
}

static void touch_point_free(struct TouchPoint* tp) {
    if (tp->surface != NULL) wl_list_remove(&tp->surface_destroy.link);
    wl_list_remove(&tp->link);
    free(tp);
}

static void touch_point_surface_destroy(struct wl_listener* l, void* data) {
    (void)data;

    struct TouchPoint* tp = wl_container_of(l, tp, surface_destroy);
    wl_list_remove(&tp->surface_destroy.link);
    tp->surface = NULL;
    wl_list_remove(&tp->link);
    free(tp);
}

static struct TouchPoint* touch_point_begin(
    int32_t touch_id, struct wlr_surface* surface, double lx, double ly,
    double sx, double sy
) {
    struct TouchPoint* old = touch_point_find(touch_id);
    if (old != NULL) touch_point_free(old);

    struct TouchPoint* tp = calloc(1, sizeof(*tp));
    if (tp == NULL) return NULL;

    tp->touch_id = touch_id;
    tp->surface = surface;
    tp->start_lx = lx;
    tp->start_ly = ly;
    tp->start_sx = sx;
    tp->start_sy = sy;

    if (surface != NULL) {
        tp->surface_destroy.notify = touch_point_surface_destroy;
        wl_signal_add(&surface->events.destroy, &tp->surface_destroy);
    }

    wl_list_insert(&touch_points, &tp->link);
    return tp;
}

void cursor_handle_touch_up(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_touch_up_event* ev = data;

    if (overview_touch_id == ev->touch_id) {
        overview_touch_id = -1;
        if (server.overview_active) {
            overview_handle_touch_up(server.cursor->x);
        }

        return;
    }

    struct TouchPoint* tp = touch_point_find(ev->touch_id);
    if (tp == NULL) return;

    const bool was_deco = (tp->surface == NULL);
    touch_point_free(tp);

    if (was_deco) {
        deco_handle_release(BTN_LEFT);
        if (interactive.active) {
            cursor_end_interactive();
            cursor_update_focus();
        }

        return;
    }

    wlr_seat_touch_notify_up(server.seat, ev->time_msec, ev->touch_id);
}

void cursor_handle_touch_cancel(struct wl_listener* l, void* data) {
    (void)l;
    (void)data;

    if (overview_touch_id != -1) {
        overview_touch_id = -1;
        overview_handle_touch_cancel();
    }

    if (interactive.active) {
        cursor_end_interactive();
        cursor_update_focus();
    }

    struct wlr_seat_client* sc;
    wl_list_for_each(sc, &server.seat->clients, link) {
        wlr_seat_touch_notify_cancel(server.seat, sc);
    }

    struct TouchPoint *tp, *tmp;
    wl_list_for_each_safe(tp, tmp, &touch_points, link) {
        touch_point_free(tp);
    }
}

void cursor_handle_touch_down(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_touch_down_event* ev = data;

    if (server.locked) return;

    if (server.overview_active) {
        // The first finger owns the drag, and we ignore all others (for now).
        if (overview_touch_id == -1) {
            double lx, ly;
            wlr_cursor_absolute_to_layout_coords(
                server.cursor, &ev->touch->base, ev->x, ev->y, &lx, &ly
            );

            wlr_cursor_warp(server.cursor, NULL, lx, ly);
            overview_touch_id = ev->touch_id;
            overview_handle_touch_down(lx, ly);
        }

        return;
    }

    server_notify_activity();
    cursor_wake();

    double lx, ly;
    wlr_cursor_absolute_to_layout_coords(
        server.cursor, &ev->touch->base, ev->x, ev->y, &lx, &ly
    );

    wlr_cursor_warp(server.cursor, NULL, lx, ly);

    if (deco_handle_press(lx, ly, BTN_LEFT, 0)) {
        touch_point_begin(ev->touch_id, NULL, lx, ly, 0, 0);
        return;
    }

    struct wlr_surface* surface = NULL;
    double sx = 0, sy = 0;
    struct Window* window = window_at(lx, ly, &surface, &sx, &sy);
    if (surface == NULL) return;

    if (window != NULL && window->mapped && !window->space_hidden) {
        seat_focus(window);
        if (!layouts_is_tiled(wm.layout)) window_raise(window);
    }

    if (touch_point_begin(ev->touch_id, surface, lx, ly, sx, sy) == NULL) {
        return;
    }

    wlr_seat_touch_notify_down(
        server.seat, surface, ev->time_msec, ev->touch_id, sx, sy
    );
}

void cursor_handle_touch_motion(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_touch_motion_event* ev = data;

    if (overview_touch_id == ev->touch_id) {
        if (!server.overview_active) {
            overview_touch_id = -1;
            return;
        }

        double lx, ly;
        wlr_cursor_absolute_to_layout_coords(
            server.cursor, &ev->touch->base, ev->x, ev->y, &lx, &ly
        );

        wlr_cursor_warp(server.cursor, NULL, lx, ly);
        overview_handle_touch_motion(lx, ly);
        return;
    }

    struct TouchPoint* tp = touch_point_find(ev->touch_id);
    if (tp == NULL) return;

    double lx, ly;
    wlr_cursor_absolute_to_layout_coords(
        server.cursor, &ev->touch->base, ev->x, ev->y, &lx, &ly
    );

    if (tp->surface == NULL) {
        // Compositor-owned gesture (decoration drag/move).
        if (interactive.active) {
            interactive_apply(lx, ly);
        } else {
            deco_handle_motion(lx);
        }

        return;
    }

    double sx = tp->start_sx + (lx - tp->start_lx);
    double sy = tp->start_sy + (ly - tp->start_ly);

    wlr_seat_touch_notify_motion(
        server.seat, ev->time_msec, ev->touch_id, sx, sy
    );
}

void cursor_handle_touch_frame(struct wl_listener* l, void* data) {
    (void)l;
    (void)data;

    wlr_seat_touch_notify_frame(server.seat);
}

void cursor_handle_motion(struct wl_listener* l, void* data) {
    (void)l;

    server_notify_activity();
    cursor_wake();
    struct wlr_pointer_motion_event* ev = data;

    if (server.relative_pointer_manager != NULL) {
        wlr_relative_pointer_manager_v1_send_relative_motion(
            server.relative_pointer_manager, server.seat,
            (uint64_t)animation_now_ms() * 1000, ev->delta_x, ev->delta_y,
            ev->unaccel_dx, ev->unaccel_dy
        );
    }

    double dx = ev->delta_x;
    double dy = ev->delta_y;

    if (!interactive.active && !server.overview_active
        && active_constraint != NULL) {
        double sx, sy;
        struct wlr_surface* under =
            surface_at(server.cursor->x, server.cursor->y, &sx, &sy);

        if (under != active_constraint->surface) return;

        double cxs, cys;
        if (!wlr_region_confine(
                &confine_region, sx, sy, sx + dx, sy + dy, &cxs, &cys
            ))
            return;

        dx = cxs - sx;
        dy = cys - sy;
    }

    wlr_cursor_move(server.cursor, &ev->pointer->base, dx, dy);

    drag_icon_update_position();

    if (server.overview_active) {
        overview_handle_motion(server.cursor->x, server.cursor->y);
        return;
    }

    if (deco_handle_motion(server.cursor->x)) return; // sliding consumes motion

    if (interactive.active) {
        interactive_apply(server.cursor->x, server.cursor->y);
        return;
    }

    cursor_manage_focus(true);
}

void cursor_handle_motion_absolute(struct wl_listener* l, void* data) {
    (void)l;

    server_notify_activity();
    cursor_wake();
    struct wlr_pointer_motion_absolute_event* ev = data;

    if (!interactive.active && !server.overview_active
        && active_constraint != NULL) {
        double tx, ty;
        wlr_cursor_absolute_to_layout_coords(
            server.cursor, &ev->pointer->base, ev->x, ev->y, &tx, &ty
        );

        double dx = tx - server.cursor->x;
        double dy = ty - server.cursor->y;
        double sx, sy;
        struct wlr_surface* under =
            surface_at(server.cursor->x, server.cursor->y, &sx, &sy);

        if (under != active_constraint->surface) return;

        double cxs, cys;
        if (!wlr_region_confine(
                &confine_region, sx, sy, sx + dx, sy + dy, &cxs, &cys
            ))
            return;

        wlr_cursor_move(server.cursor, &ev->pointer->base, cxs - sx, cys - sy);
    } else {
        wlr_cursor_warp_absolute(
            server.cursor, &ev->pointer->base, ev->x, ev->y
        );
    }

    drag_icon_update_position();
    if (server.overview_active) {
        overview_handle_motion(server.cursor->x, server.cursor->y);
        return;
    }

    if (deco_handle_motion(server.cursor->x)) return;

    if (interactive.active) {
        interactive_apply(server.cursor->x, server.cursor->y);
        return;
    }

    cursor_manage_focus(true);
}

void cursor_handle_button(struct wl_listener* l, void* data) {
    (void)l;

    server_notify_activity();
    cursor_wake();
    struct wlr_pointer_button_event* ev = data;

    if (server.overview_active) {
        overview_handle_button(ev);
        return;
    }

    const bool pressed = ev->state == WL_POINTER_BUTTON_STATE_PRESSED;
    const uint32_t mods = current_keyboard_modifiers();
    // Release bookkeeping runs on every release.
    const bool deco_release = !pressed && deco_handle_release(ev->button);

    // Active interactive move/resize has priority.
    if (interactive.active) {
        if (!pressed
            && (interactive.trigger_button == 0
                || ev->button == interactive.trigger_button)) {
            cursor_end_interactive();
            buttons_down = 0;
            cursor_update_focus();
        }

        return;
    }

    if (deco_release) {
        cursor_update_focus();
        return;
    }

    // Floating mode
    if (pressed && !server.locked && !layouts_is_tiled(wm.layout)
        && deco_handle_press(
            server.cursor->x, server.cursor->y, ev->button, mods
        )) {
        if (interactive.active && interactive.trigger_button == 0) {
            // A tab drag may have started a move—bind it to this button.
            interactive.trigger_button = ev->button;
        }

        return;
    }

    // Pointer bindings (SUPER+click move/resize)
    if (pressed && !server.locked && !layouts_is_tiled(wm.layout)) {
        for (size_t i = 0; i < cfg_pointer_binds_len; i++) {
            const struct PointerDef* pb = &cfg_pointer_binds[i];

            if (pb->button != ev->button) continue;
            if (pb->mods != mods) continue;

            struct wlr_surface* surf = NULL;
            double sx = 0, sy = 0;
            struct Window* under =
                window_at(server.cursor->x, server.cursor->y, &surf, &sx, &sy);

            if (under == NULL || under->closed || under->space_hidden) break;

            focus_window(under);

            if (pb->action == ACTION_MOVE) {
                cursor_begin_interactive(under, SEAT_OP_MOVE, 0);
                interactive.trigger_button = ev->button;
            } else if (pb->action == ACTION_RESIZE) {
                int32_t w = under->width > 0 ? under->width : 1;
                int32_t h = under->height > 0 ? under->height : 1;
                double lx = server.cursor->x - under->x;
                double ly = server.cursor->y - under->y;

                uint32_t edges =
                    (lx < w / 2.0 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT)
                    | (ly < h / 2.0 ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);

                cursor_begin_interactive(under, SEAT_OP_RESIZE, edges);
                interactive.trigger_button = ev->button;
            }

            return;
        }
    }

    // Plain left-click on a floating window's edge starts a resize.
    if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED && !server.locked
        && ev->button == BTN_LEFT && mods == 0) {
        struct wlr_surface* surf = NULL;
        double sx = 0, sy = 0;
        struct Window* under =
            window_at(server.cursor->x, server.cursor->y, &surf, &sx, &sy);

        uint32_t edges = edge_zone(under, server.cursor->x, server.cursor->y);
        if (edges != 0) {
            focus_window(under); // no-op if already focused
            cursor_begin_interactive(under, SEAT_OP_RESIZE, edges);
            interactive.trigger_button = ev->button;
            return;
        }
    }

    // On-press focus
    if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED && !server.locked) {
        struct wlr_surface* surf = NULL;
        double sx = 0, sy = 0;
        struct Window* under =
            window_at(server.cursor->x, server.cursor->y, &surf, &sx, &sy);

        focus_window(under);

        if (under != NULL && !under->closed && !under->space_hidden) {
            window_raise(under);
        }
    }

    // Store pointer origins
    if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED && buttons_down == 0) {
        struct wlr_surface* surf = NULL;
        double sx = 0, sy = 0;
        struct Window* under =
            window_at(server.cursor->x, server.cursor->y, &surf, &sx, &sy);

        if (under != NULL && !under->closed && !under->space_hidden
            && surf != NULL) {
            pointer_grab_window = under;
            pointer_grab_lx = server.cursor->x;
            pointer_grab_ly = server.cursor->y;
            pointer_grab_sx = sx;
            pointer_grab_sy = sy;
        }
    }

    wlr_seat_pointer_notify_button(
        server.seat, ev->time_msec, ev->button, ev->state
    );

    if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        buttons_down++;
        session_lock_click_focus();
        cursor_update_focus();
    } else if (buttons_down > 0) {
        buttons_down--;
        if (buttons_down == 0) pointer_grab_window = NULL;
    }
}

void cursor_handle_axis(struct wl_listener* l, void* data) {
    (void)l;

    server_notify_activity();
    cursor_wake();
    if (interactive.active) return;

    struct wlr_pointer_axis_event* ev = data;

    // Stale accumulators
    if (server.overview_active != space_scroll_overview) {
        space_scroll_overview = server.overview_active;
        space_scroll_accum = space_scroll_h_accum = 0.0;
    }

    if (server.overview_active && !server.locked) {
        if (ev->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) {
            space_scroll_accum += ev->delta;
            if (space_scroll_accum >= SPACE_SCROLL_STEP) {
                overview_navigate(0, +1);
                space_scroll_accum = 0.0;
            } else if (space_scroll_accum <= -SPACE_SCROLL_STEP) {
                overview_navigate(0, -1);
                space_scroll_accum = 0.0;
            }

            return;
        }

        if (ev->orientation == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
            space_scroll_h_accum += ev->delta;
            if (space_scroll_h_accum >= SPACE_SCROLL_STEP) {
                overview_navigate(+1, 0);
                space_scroll_h_accum = 0.0;
            } else if (space_scroll_h_accum <= -SPACE_SCROLL_STEP) {
                overview_navigate(-1, 0);
                space_scroll_h_accum = 0.0;
            }

            return;
        }
    }

    wlr_seat_pointer_notify_axis(
        server.seat, ev->time_msec, ev->orientation, ev->delta,
        ev->delta_discrete, ev->source, ev->relative_direction
    );
}

void cursor_handle_swipe_begin(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_pointer_swipe_begin_event* ev = data;

    swipe_forwarded = false;
    swipe_space = false;
    swipe_accum = 0.0;

    if (server.locked || interactive.active || server.overview_active) return;

    if (ev->fingers == SPACE_SWIPE_FINGERS
        && !wlr_seat_pointer_has_grab(server.seat)) {
        swipe_space = true;
        return;
    }

    if (server.pointer_gestures != NULL) {
        wlr_pointer_gestures_v1_send_swipe_begin(
            server.pointer_gestures, server.seat, ev->time_msec, ev->fingers
        );

        swipe_forwarded = true;
    }
}

void cursor_handle_swipe_update(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_pointer_swipe_update_event* ev = data;

    if (swipe_forwarded) {
        wlr_pointer_gestures_v1_send_swipe_update(
            server.pointer_gestures, server.seat, ev->time_msec, ev->dx, ev->dy
        );

        return;
    }

    if (!swipe_space) return;

    swipe_accum += ev->dx;
    if (swipe_accum >= SPACE_SWIPE_STEP) {
        cursor_switch_space(+1);
        swipe_accum -= SPACE_SWIPE_STEP;
    } else if (swipe_accum <= -SPACE_SWIPE_STEP) {
        cursor_switch_space(-1);
        swipe_accum += SPACE_SWIPE_STEP;
    }
}

void cursor_handle_swipe_end(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_pointer_swipe_end_event* ev = data;

    if (swipe_forwarded && server.pointer_gestures != NULL) {
        wlr_pointer_gestures_v1_send_swipe_end(
            server.pointer_gestures, server.seat, ev->time_msec, ev->cancelled
        );
    }

    swipe_forwarded = false;
    swipe_space = false;
    swipe_accum = 0.0;
}

void cursor_handle_pinch_begin(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_pointer_pinch_begin_event* ev = data;
    if (server.pointer_gestures == NULL || server.overview_active
        || server.locked)
        return;

    wlr_pointer_gestures_v1_send_pinch_begin(
        server.pointer_gestures, server.seat, ev->time_msec, ev->fingers
    );
}

void cursor_handle_pinch_update(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_pointer_pinch_update_event* ev = data;
    if (server.pointer_gestures == NULL || server.overview_active
        || server.locked)
        return;

    wlr_pointer_gestures_v1_send_pinch_update(
        server.pointer_gestures, server.seat, ev->time_msec, ev->dx, ev->dy,
        ev->scale, ev->rotation
    );
}

void cursor_handle_pinch_end(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_pointer_pinch_end_event* ev = data;
    if (server.pointer_gestures == NULL || server.overview_active
        || server.locked)
        return;

    wlr_pointer_gestures_v1_send_pinch_end(
        server.pointer_gestures, server.seat, ev->time_msec, ev->cancelled
    );
}

void cursor_handle_hold_begin(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_pointer_hold_begin_event* ev = data;
    if (server.pointer_gestures == NULL || server.overview_active
        || server.locked)
        return;

    wlr_pointer_gestures_v1_send_hold_begin(
        server.pointer_gestures, server.seat, ev->time_msec, ev->fingers
    );
}

void cursor_handle_hold_end(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_pointer_hold_end_event* ev = data;
    if (server.pointer_gestures == NULL || server.overview_active
        || server.locked)
        return;

    wlr_pointer_gestures_v1_send_hold_end(
        server.pointer_gestures, server.seat, ev->time_msec, ev->cancelled
    );
}

void cursor_handle_frame(struct wl_listener* l, void* data) {
    (void)l;
    (void)data;

    wlr_seat_pointer_notify_frame(server.seat);
}
