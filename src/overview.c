#include "overview.h"

#include "animation.h"
#include "config.h"
#include "decoration.h"
#include "layout/layout.h"
#include "log.h"
#include "manager.h"
#include "output.h"
#include "seat.h"
#include "server.h"
#include "window.h"

#include <dev/evdev/input-event-codes.h>
#include <math.h>
#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#define LOG_TOPIC           "overview"

#define OVERVIEW_ZOOM       0.64 // perspective
#define OVERVIEW_HOVER      1.15 // highlight|focus
#define OVERVIEW_MARGIN     20
#define OVERVIEW_GAP        20 // between windows
#define OVERVIEW_SPACE_GAP  40 // between spaces
#define OV_ANIM_MS          200
#define OV_CLOSING_TICK_MS  8
#define DRAG_THRESHOLD      4.0
#define MAX_WINDOWS         256
#define EDGE_BAND_PX        200
#define EDGE_SHIFT_COOLDOWN 300 // ms between repeat shifts

struct ScaleCtx {
    double z;
    int wx, wy; // absolute position of the window's scene tree
};

typedef void (*BufferFn)(struct wlr_scene_buffer*, void*);

// DRAG
static struct Window* drag_window;
static double drag_start_x, drag_start_y;
static double drag_off_x, drag_off_y;
static bool drag_active; // threshold crossed
static int drag_target_space = -1;
static int drag_last_gap_idx = -1;
static bool edge_armed_top, edge_armed_bottom;
static int64_t last_edge_shift_ms;

// ANIMATION
static int64_t ov_anim_start;
static bool ov_animating;
static bool ov_closing; // zoom-in animation after `overview_exit()`
static struct wl_event_source* ov_closing_timer; // zoom-in driver

static int32_t scr_w, scr_h;
static int32_t ov_row_y[SPACE_COUNT];
static int32_t ov_row_h[SPACE_COUNT];

static bool ov_visible(const struct Window* w) {
    return !w->closed && w->mapped && w->scene_tree != NULL && w->space >= 0
        && w->space < SPACE_COUNT;
}

static bool ov_managed(const struct Window* w) {
    return ov_visible(w) && w->saved_parent_tree != NULL;
}

static int32_t scaled(int32_t v, double s) {
    const int32_t r = (int32_t)lround((double)v * s);
    return r < 1 ? 1 : r;
}

static int32_t lerp_i(int32_t a, int32_t b, double t) {
    return a + (int32_t)lround((double)(b - a) * t);
}

static int32_t clamp_i(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static double anim_progress(double* eased) {
    const double t = (double)(animation_now_ms() - ov_anim_start) / OV_ANIM_MS;
    *eased = t >= 1.0 ? 1.0 : animation_ease(t > 0.0 ? t : 0.0, ANIM_MOVE);
    return t;
}

static void overview_schedule_frame(void) {
    struct Output* out;
    wl_list_for_each(out, &server.outputs, link) {
        if (!out->removed && out->wlr_output != NULL) {
            wlr_output_schedule_frame(out->wlr_output);
        }
    }
}

static void ov_cursor(const char* name) {
    wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, name);
}

static struct Window* first_in_space(int space) {
    struct Window* w;
    wl_list_for_each(w, &wm.windows, link) {
        if (ov_managed(w) && w->space == space) return w;
    }

    return NULL;
}

static size_t collect_windows(int space, struct Window** out, size_t max) {
    size_t n = 0;
    struct Window* w;
    wl_list_for_each(w, &wm.windows, link) {
        if (!ov_managed(w) || w->space != space) continue;
        if (n == max) break;
        out[n++] = w;
    }

    return n;
}

static struct Window* space_neighbor(const struct Window* f, int dx) {
    struct Window* wins[MAX_WINDOWS];
    const size_t n = collect_windows(f->space, wins, MAX_WINDOWS);

    for (size_t i = 0; i < n; i++) {
        if (wins[i] != f) continue;
        const long ni = (long)i + dx;
        return (ni < 0 || ni >= (long)n) ? NULL : wins[ni];
    }

    return NULL;
}

static int view_space(void) {
    // During drag, follow the drop target so edge-scroll is visible.
    if (drag_active && drag_target_space >= 0) return drag_target_space;

    const struct Window* f = wm.overview_focused;
    if (f != NULL && ov_managed(f)) return f->space;

    const struct Output* o = output_focused();
    const int s = o != NULL ? o->current_space : 0;
    return (s >= 0 && s < SPACE_COUNT) ? s : 0;
}

// Compute the insertion index of the drag gap in `target_space`, based on the
// cursor's horizontal position and the row's current target layout.
static int compute_gap_idx(int target_space) {
    if (!drag_active || drag_window == NULL || target_space < 0) return 0;

    int idx = 0;
    struct Window* w;
    wl_list_for_each(w, &wm.windows, link) {
        if (!ov_managed(w) || w->space != target_space) continue;
        if (w == drag_window) continue;

        const int32_t sw = scaled(w->width, (double)w->ov_scale_to);
        if (server.cursor->x < w->ov_target_x + sw / 2) return idx;
        idx++;
    }

    return idx;
}

static void walk_buffers(struct wlr_scene_node* node, BufferFn fn, void* data) {
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        fn(wlr_scene_buffer_from_node(node), data);
    } else if (node->type == WLR_SCENE_NODE_TREE) {
        struct wlr_scene_tree* tree = wlr_scene_tree_from_node(node);
        struct wlr_scene_node* child;
        wl_list_for_each(child, &tree->children, link) {
            walk_buffers(child, fn, data);
        }
    }
}

static void scale_buf_cb(struct wlr_scene_buffer* buf, void* data) {
    const struct ScaleCtx* c = data;
    struct wlr_scene_surface* ss = wlr_scene_surface_try_from_buffer(buf);
    if (ss == NULL) return;

    const struct wlr_surface_state* st = &ss->surface->current;
    int w = (int)lround((double)st->width * c->z);
    int h = (int)lround((double)st->height * c->z);
    if (st->width > 0 && w < 1) w = 1;
    if (st->height > 0 && h < 1) h = 1;
    wlr_scene_buffer_set_dest_size(buf, w, h);

    // TODO client opaque region is in unscaled coords (wlr does not scale it).
    pixman_region32_t none;
    pixman_region32_init(&none);
    wlr_scene_buffer_set_opaque_region(buf, &none);
    pixman_region32_fini(&none);

    int px, py;
    wlr_scene_node_coords(&buf->node.parent->node, &px, &py);
    wlr_scene_node_set_position(
        &buf->node, (int)lround((double)(px - c->wx) * (c->z - 1.0)),
        (int)lround((double)(py - c->wy) * (c->z - 1.0))
    );
}

static void scale_window(struct Window* w, float z) {
    struct ScaleCtx c = {.z = (double)z};
    wlr_scene_node_coords(&w->scene_tree->node, &c.wx, &c.wy);
    walk_buffers(&w->scene_tree->node, scale_buf_cb, &c);
}

static void scale_on_commit(struct wl_listener* l, void* data) {
    (void)data;

    struct Window* w = wl_container_of(l, w, ov_scale_commit);
    if (!server.overview_active && !ov_closing) return;
    if (w->scene_tree != NULL) scale_window(w, w->ov_scale);
}

static bool ov_metrics(void) {
    const struct Output* out = output_focused();
    int w = 0, h = 0;
    if (out != NULL && out->wlr_output != NULL) {
        wlr_output_effective_resolution(out->wlr_output, &w, &h);
    }

    if (w <= 0 || h <= 0) {
        log_warn("no usable output for overview");
        return false;
    }

    scr_w = w;
    scr_h = h;

    return true;
}

static void compute_targets(void) {
    const struct Window* focus = wm.overview_focused;
    struct Window* w;

    const bool inserting =
        drag_active && drag_window != NULL && drag_target_space >= 0;

    // All non-dragged windows contribute to their own space; the dragged window
    // contributes to the target space (so the row can hold it on release).
    for (int s = 0; s < SPACE_COUNT; s++) ov_row_h[s] = 1;
    wl_list_for_each(w, &wm.windows, link) {
        if (!ov_managed(w)) continue;
        if (drag_active && w == drag_window) continue;
        const int32_t h = scaled(w->height, OVERVIEW_ZOOM * OVERVIEW_HOVER);
        if (h > ov_row_h[w->space]) ov_row_h[w->space] = h;
    }
    if (inserting) {
        const int32_t h =
            scaled(drag_window->height, OVERVIEW_ZOOM * OVERVIEW_HOVER);
        if (h > ov_row_h[drag_target_space]) {
            ov_row_h[drag_target_space] = h;
        }
    }

    int32_t y = 0;
    for (int s = 0; s < SPACE_COUNT; s++) {
        ov_row_y[s] = y;
        y += ov_row_h[s] + OVERVIEW_SPACE_GAP;
    }

    const int view = view_space();
    const int32_t dy = scr_h / 2 - (ov_row_y[view] + ov_row_h[view] / 2);
    for (int s = 0; s < SPACE_COUNT; s++) ov_row_y[s] += dy;

    // Horizontal layout, per space. Two passes:
    // 1. Compute total row width (including the drag gap when inserting) and
    //    the focus window's center (also gap-aware).
    // 2. Layout each window left-to-right, advancing `cx` by the gap size when
    //    we reach `drag_last_gap_idx`.
    //
    // Computing x0 from the GAPPED total (not the ungapped one) keeps the row
    // from re-centering as the cursor slides horizontally within the row.
    for (int s = 0; s < SPACE_COUNT; s++) {
        const bool row_inserting = inserting && s == drag_target_space;
        const int32_t drag_sw =
            row_inserting
                ? scaled(drag_window->width, OVERVIEW_ZOOM * OVERVIEW_HOVER)
                : 0;

        // Pass 1 (total width (with gap) and focus center)
        int32_t x = 0, focus_cx = -1;
        int32_t idx = 0;
        bool gap_placed = !row_inserting;
        wl_list_for_each(w, &wm.windows, link) {
            if (!ov_managed(w) || w->space != s) continue;
            if (drag_active && w == drag_window) continue;

            if (!gap_placed && idx == drag_last_gap_idx) {
                x += drag_sw + OVERVIEW_GAP;
                gap_placed = true;
            }

            const double sc =
                (w == focus) ? OVERVIEW_ZOOM * OVERVIEW_HOVER : OVERVIEW_ZOOM;

            const int32_t sw = scaled(w->width, sc);
            if (w == focus) focus_cx = x + sw / 2;
            x += sw + OVERVIEW_GAP;
            idx++;
        }

        if (!gap_placed) x += drag_sw + OVERVIEW_GAP;

        const int32_t total = (x > 0) ? x - OVERVIEW_GAP : 0;
        if (total == 0) continue;

        int32_t x0 = OVERVIEW_MARGIN;
        if (total <= scr_w - 2 * OVERVIEW_MARGIN) {
            x0 = (scr_w - total) / 2;
        } else if (focus_cx >= 0) {
            x0 = clamp_i(
                scr_w / 2 - focus_cx, scr_w - OVERVIEW_MARGIN - total,
                OVERVIEW_MARGIN
            );
        }

        // Pass 2 (place windows, skipping over the gap slot)
        int32_t cx = x0;
        idx = 0;
        gap_placed = !row_inserting;
        wl_list_for_each(w, &wm.windows, link) {
            if (!ov_managed(w) || w->space != s) continue;
            if (drag_active && w == drag_window) continue;

            if (!gap_placed && idx == drag_last_gap_idx) {
                cx += drag_sw + OVERVIEW_GAP;
                gap_placed = true;
            }

            const double sc =
                (w == focus) ? OVERVIEW_ZOOM * OVERVIEW_HOVER : OVERVIEW_ZOOM;
            const int32_t sw = scaled(w->width, sc);
            const int32_t sh = scaled(w->height, sc);

            w->ov_scale_to = (float)sc;
            w->ov_target_x = cx;
            w->ov_target_y = ov_row_y[s] + (ov_row_h[s] - sh) / 2;
            cx += sw + OVERVIEW_GAP;
            idx++;
        }
    }
}

static void ov_start_anim(void) {
    struct Window* w;
    wl_list_for_each(w, &wm.windows, link) {
        if (!ov_managed(w)) continue;
        w->ov_from_x = w->x;
        w->ov_from_y = w->y;
        w->ov_scale_from = w->ov_scale;
    }

    ov_anim_start = animation_now_ms();
    ov_animating = true;
    overview_schedule_frame();
}

void overview_layout(void) {
    if (!server.overview_active) return;
    compute_targets();
    ov_start_anim();
}

static void ov_focus(struct Window* w) {
    if (w == wm.overview_focused) return;
    wm.overview_focused = w;
    if (w != NULL) wlr_scene_node_raise_to_top(&w->scene_tree->node);
    overview_layout();
}

static bool ov_tree_ok(void) {
    if (!server.overview_tree) {
        log_warn("overview_tree is NULL");
        return false;
    }

    return true;
}

static bool closing_step(void) {
    double e;
    const bool done = anim_progress(&e) >= 1.0;

    struct Window* w;
    wl_list_for_each(w, &wm.windows, link) {
        if (done && w->ov_scale_hooked) {
            wl_list_remove(&w->ov_scale_commit.link);
            w->ov_scale_hooked = false;
        }

        if (w->saved_parent_tree == NULL) continue;

        const bool vis = ov_visible(w);
        if (vis) {
            w->ov_scale = w->ov_scale_from
                        + (w->ov_scale_to - w->ov_scale_from) * (float)e;

            scale_window(w, w->ov_scale);
        }

        if (done) {
            w->saved_parent_tree = NULL;
            if (vis) deco_set_visible(w, true);
        }
    }

    if (done) ov_closing = false;
    return !done;
}

static void closing_stop(void) {
    if (ov_closing_timer == NULL) return;
    wl_event_source_remove(ov_closing_timer);
    ov_closing_timer = NULL;
}

static int closing_timer_cb(void* data) {
    (void)data;

    if (closing_step()) {
        wl_event_source_timer_update(ov_closing_timer, OV_CLOSING_TICK_MS);
    } else {
        closing_stop();
    }

    return 0;
}

static void closing_start(void) {
    ov_closing = true;
    ov_anim_start = animation_now_ms();

    struct wl_event_loop* loop = wl_display_get_event_loop(server.display);
    ov_closing_timer = wl_event_loop_add_timer(loop, closing_timer_cb, NULL);
    if (ov_closing_timer == NULL) {
        ov_anim_start -= OV_ANIM_MS;
        closing_step();
        return;
    }

    wl_event_source_timer_update(ov_closing_timer, OV_CLOSING_TICK_MS);
}

void overview_enter(void) {
    if (server.overview_active) return;
    if (!ov_tree_ok() || !ov_metrics()) return;

    closing_stop();
    ov_closing = false;
    server.overview_active = true;
    drag_window = NULL;
    drag_active = false;
    drag_target_space = -1;
    drag_last_gap_idx = -1;
    edge_armed_top = edge_armed_bottom = false;
    last_edge_shift_ms = 0;

    wlr_scene_node_set_enabled(&server.bottom_tree->node, false);
    wlr_scene_node_set_enabled(&server.tile_tree->node, false);
    wlr_scene_node_set_enabled(&server.float_tree->node, false);
    wlr_scene_node_set_enabled(&server.top_tree->node, false);
    wlr_scene_node_set_enabled(&server.fullscreen_tree->node, false);
    wlr_scene_node_set_enabled(&server.popup_tree->node, false);
    wlr_scene_node_set_enabled(&server.overlay_tree->node, false);
    wlr_scene_node_set_enabled(&server.overview_tree->node, true);

    wlr_seat_keyboard_notify_clear_focus(server.seat);
    wlr_seat_pointer_clear_focus(server.seat);
    ov_cursor("default");

    struct Window* w;
    wl_list_for_each(w, &wm.windows, link) {
        if (!ov_visible(w)) continue;

        w->saved_parent_tree = w->scene_tree->node.parent;
        w->anim.running = false;
        w->has_target = false;
        w->hide_dir = 0;
        w->reveal_dir = 0;

        wlr_scene_node_reparent(&w->scene_tree->node, server.overview_tree);
        deco_set_visible(w, false);

        window_clear_clip(w);

        if (w->ov_scale <= 0.0f) w->ov_scale = 1.0f;
        scale_window(w, w->ov_scale);

        struct wlr_surface* surf = window_wlr_surface(w);
        if (surf && !w->ov_scale_hooked) {
            w->ov_scale_commit.notify = scale_on_commit;
            wl_signal_add(&surf->events.commit, &w->ov_scale_commit);
            w->ov_scale_hooked = true;
        }
    }

    struct Window* top = wm_focus_stack_top();
    wm.overview_focused = (top != NULL && ov_managed(top)) ? top : NULL;
    if (wm.overview_focused != NULL) {
        wlr_scene_node_raise_to_top(&wm.overview_focused->scene_tree->node);
    }

    compute_targets();

    const int32_t cx = scr_w / 2, cy = scr_h / 2;
    wl_list_for_each(w, &wm.windows, link) {
        if (!ov_managed(w)) continue;
        if (w->space_hidden) {
            w->x = cx + (int32_t)lround((w->ov_target_x - cx) / OVERVIEW_ZOOM);
            w->y = cy + (int32_t)lround((w->ov_target_y - cy) / OVERVIEW_ZOOM);
            w->ov_scale = 1.0f;
            scale_window(w, 1.0);
        }

        w->pos_valid = true;
        wlr_scene_node_set_position(&w->scene_tree->node, w->x, w->y);
    }

    ov_start_anim();
}

void overview_exit(void) {
    if (!server.overview_active) return;

    struct Window* to_focus = wm.overview_focused;
    server.overview_active = false;
    wm.overview_focused = NULL;
    drag_window = NULL;
    drag_active = false;
    drag_target_space = -1;
    drag_last_gap_idx = -1;
    edge_armed_top = edge_armed_bottom = false;
    ov_animating = false;

    wlr_scene_node_set_enabled(&server.bottom_tree->node, true);
    wlr_scene_node_set_enabled(&server.tile_tree->node, true);
    wlr_scene_node_set_enabled(&server.float_tree->node, true);
    wlr_scene_node_set_enabled(&server.top_tree->node, true);
    wlr_scene_node_set_enabled(&server.fullscreen_tree->node, true);
    wlr_scene_node_set_enabled(&server.popup_tree->node, true);
    wlr_scene_node_set_enabled(&server.overlay_tree->node, true);
    wlr_scene_node_set_enabled(&server.overview_tree->node, false);

    struct Window* w;
    wl_list_for_each(w, &wm.windows, link) {
        if (!ov_managed(w)) continue;

        wlr_scene_node_reparent(&w->scene_tree->node, w->saved_parent_tree);
        w->ov_scale_from = w->ov_scale;
        w->ov_scale_to = 1.0f;
        w->has_target = false;
        w->anim.running = false;
    }

    if (to_focus && !to_focus->closed && to_focus->output
        && to_focus->output->current_space != to_focus->space) {
        wm_switch_space_on(to_focus->output, to_focus->space);
    }

    seat_focus(to_focus);

    wl_list_for_each(w, &wm.windows, link) {
        if (!ov_managed(w)) continue;

        int current;
        if (w->output != NULL) {
            current = w->output->current_space;
        } else {
            struct Output* fo = output_focused();
            if (fo == NULL) continue;
            current = fo->current_space;
        }

        const bool should_hide = (w->space != current);
        w->space_hidden = should_hide;
        if (!should_hide) continue;

        w->ov_scale = w->ov_scale_from = w->ov_scale_to = 1.0f;
        scale_window(w, 1.0);
        w->x = HIDDEN_POS_X;
        wlr_scene_node_set_position(&w->scene_tree->node, HIDDEN_POS_X, w->y);

        if (w->popup_tree) {
            wlr_scene_node_set_position(
                &w->popup_tree->node, HIDDEN_POS_X, w->y
            );
        }
    }

    wm_mark_layout_dirty();
    closing_start();
}

bool overview_tick(void) {
    if (!server.overview_active) return false;

    if (ov_animating) {
        double e;
        if (anim_progress(&e) >= 1.0) ov_animating = false;

        struct Window* w;
        wl_list_for_each(w, &wm.windows, link) {
            if (!ov_managed(w)) continue;
            if (drag_active && w == drag_window)
                continue; // position from motion
            w->x = lerp_i(w->ov_from_x, w->ov_target_x, e);
            w->y = lerp_i(w->ov_from_y, w->ov_target_y, e);
            w->ov_scale = w->ov_scale_from
                        + (w->ov_scale_to - w->ov_scale_from) * (float)e;
        }
    }

    struct Window* w;
    wl_list_for_each(w, &wm.windows, link) {
        if (!ov_managed(w)) continue;
        if (drag_active && w == drag_window) {
            // Dragged window is hovered (scale), positioned by motion handler.
            w->ov_scale = (float)(OVERVIEW_ZOOM * OVERVIEW_HOVER);
        }

        scale_window(w, w->ov_scale);
        wlr_scene_node_set_position(&w->scene_tree->node, w->x, w->y);
    }

    return ov_animating;
}

bool overview_is_animating(void) {
    return server.overview_active && ov_animating;
}

static void ov_navigate(int dx, int dy) {
    const struct Window* f = wm.overview_focused;
    if (!f) return;

    struct Window* target = NULL;
    if (dx != 0) {
        target = space_neighbor(f, dx);
    } else {
        for (int s = f->space + dy; s >= 0 && s < SPACE_COUNT; s += dy) {
            target = first_in_space(s);
            if (target) break;
        }
    }

    if (target) ov_focus(target);
}

static void ov_move_window(int dx, int dy) {
    struct Window* f = wm.overview_focused;
    if (!f) return;

    if (dx != 0) {
        struct Window* other = space_neighbor(f, dx);
        if (!other) return;

        wl_list_remove(&f->link);
        wl_list_insert(dx > 0 ? &other->link : other->link.prev, &f->link);
    } else {
        const int ns = f->space + dy;
        if (ns < 0 || ns >= SPACE_COUNT) return;
        f->space = ns;
    }

    overview_layout();
}

bool overview_handle_key(
    uint32_t keycode, const xkb_keysym_t* syms, int nsyms, uint32_t mods
) {
    (void)keycode;
    if (nsyms <= 0) return false;

    const xkb_keysym_t sym = xkb_keysym_to_lower(syms[0]);

    if ((mods == CFG_MOD_SUPER && sym == XKB_KEY_Tab)
        || (mods == 0 && sym == XKB_KEY_Escape)) {
        overview_exit();
        return true;
    }

    int dx = 0, dy = 0;
    switch (sym) {
        case XKB_KEY_Left:
        case XKB_KEY_h: dx = -1; break;
        case XKB_KEY_Right:
        case XKB_KEY_l: dx = 1; break;
        case XKB_KEY_Up:
        case XKB_KEY_k: dy = -1; break;
        case XKB_KEY_Down:
        case XKB_KEY_j: dy = 1; break;
        case XKB_KEY_Return: overview_exit(); return true;
        default: return false;
    }

    if (mods & WLR_MODIFIER_SHIFT)
        ov_move_window(dx, dy);
    else
        ov_navigate(dx, dy);

    return true;
}

static struct Window* win_at(double lx, double ly) {
    struct Window* w;
    wl_list_for_each_reverse(w, &wm.windows, link) {
        if (!ov_managed(w)) continue;
        if (lx >= w->x && lx < w->x + scaled(w->width, (double)w->ov_scale)
            && ly >= w->y
            && ly < w->y + scaled(w->height, (double)w->ov_scale)) {
            return w;
        }
    }

    return NULL;
}

bool overview_handle_button(struct wlr_pointer_button_event* ev) {
    if (!server.overview_active) return false;
    if (ev->button != BTN_LEFT) return true;

    const double cx = server.cursor->x;
    const double cy = server.cursor->y;

    if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        struct Window* hit = win_at(cx, cy);
        if (hit) {
            drag_window = hit;
            drag_active = false;
            drag_target_space = -1;
            drag_last_gap_idx = -1;
            edge_armed_top = edge_armed_bottom = false;
            drag_start_x = cx;
            drag_start_y = cy;
            ov_focus(hit);
            drag_off_x = cx - hit->x;
            drag_off_y = cy - hit->y;
        }

        return true;
    }

    if (ev->state != WL_POINTER_BUTTON_STATE_RELEASED) return true;

    struct Window* dragged = drag_window;
    const bool was_dragging = drag_active;
    const int target = drag_target_space;

    drag_window = NULL;
    drag_active = false;
    drag_target_space = -1;
    drag_last_gap_idx = -1;
    edge_armed_top = edge_armed_bottom = false;

    if (!dragged) { // click on empty
        overview_exit();
        return true;
    }

    if (!was_dragging) { // plain click—zoom into the window's space
        overview_exit();
        if (!layouts_is_tiled(wm.layout)) window_raise(dragged);
        return true;
    }

    // Commit the drag into the target space at the anchor position.
    ov_cursor("default");
    wl_list_remove(&dragged->link);

    struct Window* anchor = NULL;
    struct Window* w;
    wl_list_for_each(w, &wm.windows, link) {
        if (!ov_managed(w) || w->space != target) continue;
        if (w->ov_target_x
                + (double)scaled(w->width, (double)w->ov_scale_to) / 2
            >= cx) {
            anchor = w;
            break;
        }
    }

    if (target >= 0) dragged->space = target;
    wl_list_insert(
        anchor ? anchor->link.prev : wm.windows.prev, &dragged->link
    );

    overview_layout();
    return true;
}

bool overview_handle_motion(double lx, double ly) {
    if (!server.overview_active) return false;

    struct Window* w = drag_window;
    if (w == NULL) { // hover focuses
        struct Window* under = win_at(lx, ly);
        if (under != NULL) ov_focus(under);
        return true;
    }

    if (!drag_active) {
        if (fabs(lx - drag_start_x) + fabs(ly - drag_start_y)
            <= DRAG_THRESHOLD) {
            return true;
        }

        // Commit to drag, detach from source row, prepare target row.
        drag_active = true;
        drag_target_space = w->space;
        drag_last_gap_idx = compute_gap_idx(drag_target_space);
        edge_armed_top = (ly >= EDGE_BAND_PX);
        edge_armed_bottom = (ly <= scr_h - EDGE_BAND_PX);
        last_edge_shift_ms = 0;
        ov_cursor("grabbing");

        overview_layout();
    }

    w->x = (int32_t)lround(lx - drag_off_x);
    w->y = (int32_t)lround(ly - drag_off_y);
    w->pos_valid = true;
    wlr_scene_node_set_position(&w->scene_tree->node, w->x, w->y);

    // Edge scroll (shift target space and view center)
    const int64_t now = animation_now_ms();
    const bool cooldown_ok = (now - last_edge_shift_ms) > EDGE_SHIFT_COOLDOWN;
    const bool in_top = (ly < EDGE_BAND_PX);
    const bool in_bottom = (ly > scr_h - EDGE_BAND_PX);

    bool relaid = false;

    if (in_top && drag_target_space > 0 && (edge_armed_top || cooldown_ok)) {
        drag_target_space--;
        drag_last_gap_idx = compute_gap_idx(drag_target_space);
        edge_armed_top = false;
        last_edge_shift_ms = now;
        overview_layout();
        relaid = true;
    } else if (!in_top) {
        edge_armed_top = true;
    }

    if (in_bottom && drag_target_space < SPACE_COUNT - 1
        && (edge_armed_bottom || cooldown_ok)) {
        drag_target_space++;
        drag_last_gap_idx = compute_gap_idx(drag_target_space);
        edge_armed_bottom = false;
        last_edge_shift_ms = now;
        overview_layout();
        relaid = true;
    } else if (!in_bottom) {
        edge_armed_bottom = true;
    }

    // Horizontal gap update (only if we did not re-layout just now)
    if (!relaid) {
        const int new_idx = compute_gap_idx(drag_target_space);
        if (new_idx != drag_last_gap_idx) {
            drag_last_gap_idx = new_idx;
            overview_layout();
        }
    }

    overview_schedule_frame();
    return true;
}

void overview_window_destroy(struct Window* window) {
    if (window->ov_scale_hooked) {
        wl_list_remove(&window->ov_scale_commit.link);
        window->ov_scale_hooked = false;
    }

    window->saved_parent_tree = NULL;
    window->ov_scale = 0.0f;

    if (drag_window == window) {
        drag_window = NULL;
        drag_active = false;
        drag_target_space = -1;
        drag_last_gap_idx = -1;
        edge_armed_top = edge_armed_bottom = false;
    }

    if (wm.overview_focused == window) wm.overview_focused = NULL;
}
