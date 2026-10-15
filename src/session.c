#include "session.h"

#include "cursor.h"
#include "log.h"
#include "manager.h"
#include "output.h"
#include "seat.h"
#include "server.h"
#include "view.h"

#include <scenefx/types/wlr_scene.h>
#include <stdint.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>

#define LOG_TOPIC "session"

struct SessionLock {
    struct wlr_session_lock_v1 *lock;
    struct wlr_scene_tree *tree;
    struct wl_list surfaces; // LockSurface.link (oldest first)
    struct wl_listener new_surface, unlock, destroy;

    bool locked_sent;
};

struct LockSurface {
    struct SessionLock *lock;
    struct wlr_session_lock_surface_v1 *surface;
    struct wlr_scene_tree *tree; // NULL once the tree is gone
    struct wl_listener destroy, tree_destroy;
    struct wl_list link;

    uint32_t width, height; // saved size
};

static struct wlr_session_lock_manager_v1 *manager;
static struct wl_listener new_lock_listener;
static struct SessionLock *current_lock;

static bool is_lock_surface(struct wlr_surface *surface) {
    if (surface == NULL || current_lock == NULL) return false;

    struct LockSurface *ls;
    wl_list_for_each(ls, &current_lock->surfaces, link) {
        if (ls->surface->surface == surface) return true;
    }

    return false;
}

static void focus_lock_surface(struct wlr_surface *surface) {
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(server.seat);
    if (kb != NULL) {
        wlr_seat_keyboard_notify_enter(
            server.seat, surface, kb->keycodes, kb->num_keycodes, &kb->modifiers
        );
    } else {
        wlr_seat_keyboard_notify_enter(server.seat, surface, NULL, 0, NULL);
    }
}

static void ensure_keyboard_focus(void) {
    if (!server.locked || current_lock == NULL) return;
    if (is_lock_surface(server.seat->keyboard_state.focused_surface)) return;
    if (wl_list_empty(&current_lock->surfaces)) {
        wlr_seat_keyboard_clear_focus(server.seat);
        return;
    }

    struct LockSurface *first =
        wl_container_of(current_lock->surfaces.next, first, link);

    focus_lock_surface(first->surface->surface);
}

static void configure_surface(struct LockSurface *ls) {
    int w, h;
    wlr_output_effective_resolution(ls->surface->output, &w, &h);
    if (w <= 0 || h <= 0) return;
    if (ls->width == (uint32_t)w && ls->height == (uint32_t)h) return;

    ls->width = (uint32_t)w;
    ls->height = (uint32_t)h;
    wlr_session_lock_surface_v1_configure(ls->surface, ls->width, ls->height);
}

void session_lock_update_geometry(void) {
    if (server.lock_bg != NULL) {
        struct wlr_box box;

        wlr_output_layout_get_box(server.output_layout, NULL, &box);
        wlr_scene_node_set_position(&server.lock_bg->node, box.x, box.y);
        wlr_scene_rect_set_size(server.lock_bg, box.width, box.height);
    }

    if (current_lock == NULL) return;

    struct LockSurface *ls;
    wl_list_for_each(ls, &current_lock->surfaces, link) {
        if (ls->tree == NULL) continue;

        struct wlr_output_layout_output *lo =
            wlr_output_layout_get(server.output_layout, ls->surface->output);
        if (lo == NULL) continue;

        wlr_scene_node_set_position(&ls->tree->node, lo->x, lo->y);
        configure_surface(ls);
    }
}

// `locked` may only be sent after every visible output has shown a blanked
// frame.
static void try_send_locked(void) {
    struct SessionLock *lock = current_lock;
    if (lock == NULL || lock->locked_sent) return;

    struct Output *o;
    wl_list_for_each(o, &server.outputs, link) {
        if (o->removed || !o->wlr_output->enabled) continue;
        if (!o->lock_blanked) return;
    }

    lock->locked_sent = true;
    wlr_session_lock_v1_send_locked(lock->lock);
}

void session_lock_output_committed(struct Output *out) {
    if (!server.locked) return;
    out->lock_blanked = true;
    try_send_locked();
}

void session_lock_outputs_changed(void) { try_send_locked(); }

void session_lock_click_focus(void) {
    if (!server.locked) return;

    struct wlr_surface *s = server.seat->pointer_state.focused_surface;
    if (s == NULL) return;

    struct wlr_surface *root = wlr_surface_get_root_surface(s);
    if (!is_lock_surface(root)) return;
    if (server.seat->keyboard_state.focused_surface == root) return;

    focus_lock_surface(root);
}

static void
lock_surface_handle_tree_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct LockSurface *ls = wl_container_of(l, ls, tree_destroy);
    wl_list_remove(&ls->tree_destroy.link);
    ls->tree = NULL;
}

static void lock_surface_handle_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct LockSurface *ls = wl_container_of(l, ls, destroy);

    wl_list_remove(&ls->destroy.link);
    wl_list_remove(&ls->link);

    if (ls->tree != NULL) wlr_scene_node_destroy(&ls->tree->node);

    free(ls);
    ensure_keyboard_focus();
}

static void lock_handle_new_surface(struct wl_listener *l, void *data) {
    struct SessionLock *lock = wl_container_of(l, lock, new_surface);
    struct wlr_session_lock_surface_v1 *surf = data;

    struct wlr_output_layout_output *lo =
        surf->output != NULL
            ? wlr_output_layout_get(server.output_layout, surf->output)
            : NULL;

    if (lo == NULL) {
        log_warn("Lock surface for an output outside the layout (ignored)");
        return;
    }

    struct LockSurface *ls = calloc(1, sizeof(*ls));
    if (ls == NULL) return;

    ls->tree = wlr_scene_subsurface_tree_create(lock->tree, surf->surface);
    if (ls->tree == NULL) {
        free(ls);
        return;
    }

    ls->lock = lock;
    ls->surface = surf;

    ls->tree_destroy.notify = lock_surface_handle_tree_destroy;
    wl_signal_add(&ls->tree->node.events.destroy, &ls->tree_destroy);

    ls->destroy.notify = lock_surface_handle_destroy;
    wl_signal_add(&surf->events.destroy, &ls->destroy);

    wl_list_insert(lock->surfaces.prev, &ls->link);
    wlr_scene_node_set_position(&ls->tree->node, lo->x, lo->y);
    configure_surface(ls);
    ensure_keyboard_focus();
}

static void release_lock(struct SessionLock *lock, bool unlock) {
    wl_list_remove(&lock->new_surface.link);
    wl_list_remove(&lock->unlock.link);
    wl_list_remove(&lock->destroy.link);

    wlr_scene_node_destroy(&lock->tree->node);

    struct LockSurface *ls, *tmp;
    wl_list_for_each_safe(ls, tmp, &lock->surfaces, link) {
        wl_list_remove(&ls->destroy.link);
        wl_list_remove(&ls->link);
        free(ls);
    }

    current_lock = NULL;
    free(lock);

    wlr_seat_keyboard_clear_focus(server.seat);

    if (!unlock) {
        log_warn("Lock client went away, session remains locked");
        return;
    }

    server.locked = false;
    wlr_scene_node_set_enabled(&server.lock_tree->node, false);
    wlr_scene_node_set_enabled(&server.content_tree->node, true);

    idle_update_inhibit();
    seat_focus(NULL);
    cursor_refresh_focus();
    wm_mark_layout_dirty();
}

static void lock_handle_unlock(struct wl_listener *l, void *data) {
    (void)data;

    struct SessionLock *lock = wl_container_of(l, lock, unlock);
    release_lock(lock, true);
}

static void lock_handle_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct SessionLock *lock = wl_container_of(l, lock, destroy);
    release_lock(lock, !lock->locked_sent);
}

static void engage_lock(void) {
    server.locked = true;

    cursor_end_interactive();
    wlr_seat_pointer_end_grab(server.seat);
    wlr_seat_keyboard_end_grab(server.seat);
    wlr_seat_pointer_clear_focus(server.seat);
    wlr_seat_keyboard_clear_focus(server.seat);

    struct Window *focused = view_focused();
    if (focused != NULL) view_set_activated(focused, false);

    wlr_scene_node_set_enabled(&server.content_tree->node, false);
    wlr_scene_node_set_enabled(&server.lock_tree->node, true);
    session_lock_update_geometry();
    idle_update_inhibit();

    struct Output *o;
    wl_list_for_each(o, &server.outputs, link) {
        o->lock_blanked = false;
        wlr_output_schedule_frame(o->wlr_output);
    }
}

static void handle_new_lock(struct wl_listener *l, void *data) {
    (void)l;

    struct wlr_session_lock_v1 *wlr_lock = data;

    if (current_lock != NULL) {
        wlr_session_lock_v1_destroy(wlr_lock);
        return;
    }

    struct SessionLock *lock = calloc(1, sizeof(*lock));
    if (lock == NULL) {
        wlr_session_lock_v1_destroy(wlr_lock);
        return;
    }

    lock->lock = wlr_lock;
    lock->tree = wlr_scene_tree_create(server.lock_tree);
    wl_list_init(&lock->surfaces);

    lock->new_surface.notify = lock_handle_new_surface;
    wl_signal_add(&wlr_lock->events.new_surface, &lock->new_surface);

    lock->unlock.notify = lock_handle_unlock;
    wl_signal_add(&wlr_lock->events.unlock, &lock->unlock);

    lock->destroy.notify = lock_handle_destroy;
    wl_signal_add(&wlr_lock->events.destroy, &lock->destroy);

    bool already_locked = server.locked; // previous locker died
    current_lock = lock;

    if (already_locked) {
        struct Output *o;
        wl_list_for_each(o, &server.outputs, link) o->lock_blanked = true;
    } else {
        engage_lock();
    }

    try_send_locked();
}

bool session_lock_init(struct wl_display *display) {
    static const float bg_color[4] = {0.05f, 0.05f, 0.05f, 1.0f};
    server.lock_bg = wlr_scene_rect_create(server.lock_tree, 0, 0, bg_color);

    manager = wlr_session_lock_manager_v1_create(display);
    if (manager == NULL) return false;

    new_lock_listener.notify = handle_new_lock;
    wl_signal_add(&manager->events.new_lock, &new_lock_listener);
    return true;
}

void session_lock_detach_listeners(void) {
    if (manager != NULL) wl_list_remove(&new_lock_listener.link);
}
