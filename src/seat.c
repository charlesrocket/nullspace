#include "seat.h"

#include "actions.h"
#include "config.h"
#include "cursor.h"
#include "decoration.h"
#include "input.h"
#include "ipc.h"
#include "keymap.h"
#include "layout/layout.h"
#include "manager.h"
#include "output.h"
#include "server.h"
#include "spawn.h"
#include "window.h"

#include <dev/evdev/input-event-codes.h>
#include <fcntl.h>
#include <math.h>
#include <nullspace/layouts.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#define LOG_TOPIC           "seat"

#define RESIZE_STEP_PX      1
#define RESIZE_MIN_PX       40
#define RESIZE_SIZE_PER_MIN 0.1f
#define RESIZE_SIZE_PER_MAX 5.0f
#define RESIZE_MFACT_MIN    0.1f
#define RESIZE_MFACT_MAX    0.9f

#define KEY_MODS_IGNORED    ((uint32_t)(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2))

#define SWALLOWED_MAX       16
static uint32_t swallowed_keys[SWALLOWED_MAX];
static size_t swallowed_len;

static void swallow_add(uint32_t keycode) {
    for (size_t i = 0; i < swallowed_len; i++)
        if (swallowed_keys[i] == keycode) return;

    if (swallowed_len < SWALLOWED_MAX)
        swallowed_keys[swallowed_len++] = keycode;
}

static bool swallow_take(uint32_t keycode) {
    for (size_t i = 0; i < swallowed_len; i++) {
        if (swallowed_keys[i] != keycode) continue;
        swallowed_keys[i] = swallowed_keys[--swallowed_len];
        return true;
    }

    return false;
}

struct DeviceEntry {
    struct wlr_input_device *device;
    struct wl_listener destroy;
    struct wl_list link;
};

static struct wl_list input_devices;

static void device_entry_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct DeviceEntry *e = wl_container_of(l, e, destroy);
    wl_list_remove(&e->destroy.link);
    wl_list_remove(&e->link);
    free(e);
}

void seat_foreach_input_device(
    void (*fn)(struct wlr_input_device *, void *), void *data
) {
    struct DeviceEntry *e;
    wl_list_for_each(e, &input_devices, link) { fn(e->device, data); }
}

void seat_spawn(const void *arg) { spawn((const char *const *)arg); }
void seat_spawn_shell(const char *cmdline) { spawn_shell(cmdline); }

static void resize_mode_adjust(int32_t dx, int32_t dy);

static struct {
    bool active;
    struct Window *window;
    int32_t base_w, base_h;
} resize_mode;

static struct wl_event_source *resize_repeat_source;
static uint32_t resize_repeat_keycode; // 0 is idle
static int32_t resize_repeat_dx;
static int32_t resize_repeat_dy;

static void resize_repeat_stop(void) {
    if (resize_repeat_source != NULL) {
        wl_event_source_timer_update(resize_repeat_source, 0);
    }
    resize_repeat_keycode = 0;
}

static int resize_repeat_fire(void *data) {
    (void)data;

    if (!resize_mode.active) {
        resize_repeat_keycode = 0;
        return 0;
    }

    if (resize_repeat_dx != 0 || resize_repeat_dy != 0) {
        resize_mode_adjust(resize_repeat_dx, resize_repeat_dy);
    }

    int rate = config.kb_rate_hz;
    if (rate <= 0) { rate = 26; }

    int interval_ms = 1000 / rate;
    if (interval_ms < 1) { interval_ms = 1; }

    wl_event_source_timer_update(resize_repeat_source, interval_ms);
    return 0;
}

static void resize_repeat_start(uint32_t keycode, int32_t dx, int32_t dy) {
    if (resize_repeat_source == NULL) { return; }

    resize_repeat_keycode = keycode;
    resize_repeat_dx = dx;
    resize_repeat_dy = dy;

    int delay = config.kb_delay_ms;
    if (delay < 0) { delay = 600; }

    wl_event_source_timer_update(resize_repeat_source, delay);
}

static void resize_mode_exit(void) {
    resize_mode.active = false;
    resize_mode.window = NULL;
    wm.resizing = false;
    resize_repeat_stop();
}

void seat_resize_mode_window_destroyed(struct Window *w) {
    if (resize_mode.active && resize_mode.window == w) resize_mode_exit();
}

static void resize_mode_toggle(void) {
    if (resize_mode.active) {
        resize_mode_exit();
        return;
    }

    struct Window *w = window_focused();
    if (w == NULL || w->closed || !w->mapped || w->space_hidden) return;
    if (w->fullscreen) return;

    int32_t bw = w->prop_valid ? w->prop_w : w->width;
    int32_t bh = w->prop_valid ? w->prop_h : w->height;
    if (bw < RESIZE_MIN_PX) bw = RESIZE_MIN_PX;
    if (bh < RESIZE_MIN_PX) bh = RESIZE_MIN_PX;

    resize_mode.active = true;
    resize_mode.window = w;
    resize_mode.base_w = bw;
    resize_mode.base_h = bh;

    wm.resizing = true;
}

static bool resize_mode_handle_key(uint32_t keycode, xkb_keysym_t sym) {
    int32_t dx = 0;
    int32_t dy = 0;

    switch (sym) {
        case XKB_KEY_h:
        case XKB_KEY_Left: dx = -RESIZE_STEP_PX; break;
        case XKB_KEY_l:
        case XKB_KEY_Right: dx = RESIZE_STEP_PX; break;
        case XKB_KEY_k:
        case XKB_KEY_Up: dy = -RESIZE_STEP_PX; break;
        case XKB_KEY_j:
        case XKB_KEY_Down: dy = RESIZE_STEP_PX; break;

        case XKB_KEY_Escape:
        case XKB_KEY_Return:
        case XKB_KEY_q: resize_mode_exit(); return true;

        default: return false;
    }

    resize_mode_adjust(dx, dy);
    resize_repeat_start(keycode, dx, dy);
    return true;
}

static bool apply_size_per_pixels(struct Window *w, int32_t px_delta) {
    if (px_delta == 0) return false;

    int32_t cur_w = (w->prop_valid && w->prop_w > 0) ? w->prop_w : w->width;
    if (cur_w < 1) cur_w = 1;

    float step = (float)px_delta * w->size_per / (float)cur_w;
    if (!isfinite(step)) return false;

    float next = w->size_per + step;
    if (!(next >= RESIZE_SIZE_PER_MIN)) next = RESIZE_SIZE_PER_MIN;
    if (next > RESIZE_SIZE_PER_MAX) next = RESIZE_SIZE_PER_MAX;

    if (px_delta > 0) {
        if (!(next > w->size_per)) return false;
    } else {
        if (!(next < w->size_per)) return false;
    }

    w->size_per = next;
    return true;
}

static bool apply_mfact_pixels(int32_t px_delta, bool vertical) {
    if (px_delta == 0) return false;

    struct Output *o = output_primary();
    if (o == NULL) return false;

    int32_t ref = vertical ? o->height : o->width;
    if (ref < 1) return false;

    int32_t gap = vertical ? wm.tiled_gap_inner_v : wm.tiled_gap_inner_h;
    float step = (float)px_delta / (float)(ref + gap);
    if (!isfinite(step)) return false;

    float next = wm.mfact + step;
    if (!(next >= RESIZE_MFACT_MIN)) next = RESIZE_MFACT_MIN;
    if (next > RESIZE_MFACT_MAX) next = RESIZE_MFACT_MAX;

    if (px_delta > 0) {
        if (!(next > wm.mfact)) return false;
    } else {
        if (!(next < wm.mfact)) return false;
    }

    wm.mfact = next;
    return true;
}

static void resize_mode_adjust(int32_t dx, int32_t dy) {
    struct Window *w = resize_mode.window;
    if (w == NULL || w->closed || !w->mapped || w->space_hidden) {
        resize_mode_exit();
        return;
    }

    if (!layouts_is_tiled(wm.layout)) { // floating mode
        resize_mode.base_w += dx;
        resize_mode.base_h += dy;

        if (resize_mode.base_w < RESIZE_MIN_PX) {
            resize_mode.base_w = RESIZE_MIN_PX;
        }
        if (resize_mode.base_h < RESIZE_MIN_PX) {
            resize_mode.base_h = RESIZE_MIN_PX;
        }

        // Clamp to the usable area (content + decoration tab).
        struct wlr_box ua;
        if (window_usable_area(w, &ua)) {
            const int32_t deco_top = window_visual_top_offset(w);

            int32_t max_h = ua.height - deco_top;
            if (max_h < 1) max_h = 1;

            if (resize_mode.base_w > ua.width) {
                resize_mode.base_w = ua.width;
            }

            int32_t avail_below = ua.y + ua.height - w->y;
            if (avail_below < 1) avail_below = 1;
            int32_t cap_h = max_h < avail_below ? max_h : avail_below;

            if (resize_mode.base_h > cap_h) { resize_mode.base_h = cap_h; }

            if (resize_mode.base_w < RESIZE_MIN_PX) {
                resize_mode.base_w = RESIZE_MIN_PX;
            }
            if (resize_mode.base_h < RESIZE_MIN_PX) {
                resize_mode.base_h = RESIZE_MIN_PX;
            }
        }

        if (resize_mode.base_w != w->prop_w
            || resize_mode.base_h != w->prop_h) {
            window_set_size(w, resize_mode.base_w, resize_mode.base_h);
        }

        return;
    }

    bool changed = false;
    switch (wm.layout) {
        case LAYOUT_VERTICAL_TILE:
            changed |= apply_size_per_pixels(w, dx);
            changed |= apply_mfact_pixels(dy, true);
            break;

        case LAYOUT_HORIZONTAL_TILE:
        case LAYOUT_HORIZONTAL_RIGHT_TILE:
            changed |= apply_mfact_pixels(dx, false);
            changed |= apply_size_per_pixels(w, dy);
            break;

        case LAYOUT_VERTICAL_GRID:
        case LAYOUT_HORIZONTAL_GRID:
            changed |= apply_size_per_pixels(w, dx + dy);
            break;

        case LAYOUT_HORIZONTAL_MONOCLE:
        case LAYOUT_FLOATING: break;
    }

    if (changed) { wm_mark_layout_dirty(); }
}

void seat_init(void) {
    wl_list_init(&input_devices);

    resize_repeat_source =
        wl_event_loop_add_timer(server.event_loop, resize_repeat_fire, NULL);

    if (resize_repeat_source == NULL) { resize_repeat_keycode = 0; }
}

void seat_finish(void) {
    resize_repeat_stop();

    if (resize_repeat_source != NULL) {
        wl_event_source_remove(resize_repeat_source);
        resize_repeat_source = NULL;
    }
}

static void seat_action(enum Action action, const void *arg) {
    switch (action) {
        case ACTION_NONE: break;
        case ACTION_SPAWN: seat_spawn(arg); break;
        case ACTION_SPAWN_SHELL: seat_spawn_shell((const char *)arg); break;
        case ACTION_CLOSE:
            {
                struct Window *focused = window_focused();
                if (focused != NULL) {
                    struct Window *next = wm_focus_stack_top_except(focused);
                    window_close(focused);

                    if (next != NULL) seat_focus(next);
                }

                break;
            }

        case ACTION_FOCUS_NEXT:
            {
                struct Window *next = wm_next_window(window_focused());
                if (next != NULL) {
                    seat_focus(next);
                    if (!layouts_is_tiled(wm.layout)) window_raise(next);
                }

                break;
            }

        case ACTION_MOVE: break;
        case ACTION_RESIZE: resize_mode_toggle(); break;
        case ACTION_FULLSCREEN:
            {
                struct Window *focused = window_focused();
                if (focused != NULL) {
                    window_set_fullscreen(focused, !focused->fullscreen);
                }

                break;
            }

        case ACTION_CYCLE_LAYOUT:
            wm_set_layout((enum Layout)((wm.layout + 1) % (LAYOUT_LAST + 1)));
            break;

        case ACTION_EXIT: wl_display_terminate(server.display); break;

        case ACTION_SPACE_0: wm_switch_space(0); break;
        case ACTION_SPACE_1: wm_switch_space(1); break;
        case ACTION_SPACE_2: wm_switch_space(2); break;
        case ACTION_SPACE_3: wm_switch_space(3); break;
        case ACTION_SPACE_4: wm_switch_space(4); break;
        case ACTION_SPACE_5: wm_switch_space(5); break;
        case ACTION_SPACE_6: wm_switch_space(6); break;
        case ACTION_SPACE_7: wm_switch_space(7); break;
        case ACTION_SPACE_8: wm_switch_space(8); break;
        case ACTION_SPACE_9: wm_switch_space(9); break;

        case ACTION_MOVE_TO_SPACE_0:
            wm_move_window_to_space(window_focused(), 0);
            break;
        case ACTION_MOVE_TO_SPACE_1:
            wm_move_window_to_space(window_focused(), 1);
            break;
        case ACTION_MOVE_TO_SPACE_2:
            wm_move_window_to_space(window_focused(), 2);
            break;
        case ACTION_MOVE_TO_SPACE_3:
            wm_move_window_to_space(window_focused(), 3);
            break;
        case ACTION_MOVE_TO_SPACE_4:
            wm_move_window_to_space(window_focused(), 4);
            break;
        case ACTION_MOVE_TO_SPACE_5:
            wm_move_window_to_space(window_focused(), 5);
            break;
        case ACTION_MOVE_TO_SPACE_6:
            wm_move_window_to_space(window_focused(), 6);
            break;
        case ACTION_MOVE_TO_SPACE_7:
            wm_move_window_to_space(window_focused(), 7);
            break;
        case ACTION_MOVE_TO_SPACE_8:
            wm_move_window_to_space(window_focused(), 8);
            break;
        case ACTION_MOVE_TO_SPACE_9:
            wm_move_window_to_space(window_focused(), 9);
            break;
    }
}

static bool handle_keybinding(xkb_keysym_t sym, uint32_t mods) {
    for (size_t i = 0; i < config.bind_count; i++) {
        const struct UserBind *ub = &config.binds[i];
        if (ub->key != sym || ub->mods != mods) continue;
        seat_action(ub->action, ub->arg);
        return true;
    }

    return false;
}

static void keyboard_handle_modifiers(struct wl_listener *l, void *data) {
    (void)data;

    server_notify_activity();
    struct Keyboard *kb = wl_container_of(l, kb, modifiers);
    wlr_seat_set_keyboard(server.seat, kb->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(
        server.seat, &kb->wlr_keyboard->modifiers
    );

    keymap_notify_layout(kb->wlr_keyboard);
}

static bool is_modifier_sym(xkb_keysym_t s) {
    return (s >= XKB_KEY_Shift_L && s <= XKB_KEY_Hyper_R)
        || s == XKB_KEY_ISO_Level3_Shift || s == XKB_KEY_Mode_switch;
}

static void keyboard_handle_key(struct wl_listener *l, void *data) {
    server_notify_activity();

    struct Keyboard *kb = wl_container_of(l, kb, key);
    struct wlr_keyboard_key_event *ev = data;

    wlr_seat_set_keyboard(server.seat, kb->wlr_keyboard);

    if (ev->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (resize_repeat_keycode != 0
            && ev->keycode == resize_repeat_keycode) {
            resize_repeat_stop();
        }

        if (swallow_take(ev->keycode)) { return; }
    }

    // xkb keycodes are evdev codes + 8.
    uint32_t keycode = ev->keycode + 8;
    const xkb_keysym_t *syms = NULL;
    int nsyms =
        xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, keycode, &syms);

    // Level-0 (unshifted) keysym for keybind lookup (yields XKB_KEY_exclam
    // instead of XKB_KEY_1).
    xkb_keysym_t layout_sym = XKB_KEY_NoSymbol;
    struct xkb_keymap *keymap = kb->wlr_keyboard->keymap;
    if (keymap != NULL) {
        xkb_layout_index_t layout =
            xkb_state_key_get_layout(kb->wlr_keyboard->xkb_state, keycode);

        const xkb_keysym_t *layout_syms = NULL;
        if (xkb_keymap_key_get_syms_by_level(
                keymap, keycode, layout, 0, &layout_syms
            )
            > 0) {
            layout_sym = layout_syms[0];
        }
    }

    uint32_t mods =
        wlr_keyboard_get_modifiers(kb->wlr_keyboard) & ~KEY_MODS_IGNORED;

    bool handled = false;

    // Only match keybinds on PRESS. The key event fires on both press and
    // release, and the keysym + modifiers are identical in both cases, (without
    // this check every binding runs twice per keystroke).
    if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED && !server.locked) {
        if (resize_mode.active) {
            if (!ev->update_state) { return; }

            bool consumed = false;

            for (int i = 0; i < nsyms; i++) {
                if (resize_mode_handle_key(ev->keycode, syms[i])) {
                    consumed = true;
                    break;
                }
            }

            if (!consumed && layout_sym != XKB_KEY_NoSymbol) {
                if (resize_mode_handle_key(ev->keycode, layout_sym)) {
                    consumed = true;
                }
            }

            swallow_add(ev->keycode);
            return;
        }

        for (int i = 0; i < nsyms; i++) {
            if (handle_keybinding(syms[i], mods)) handled = true;
        }

        if (!handled && layout_sym != XKB_KEY_NoSymbol) {
            if (handle_keybinding(layout_sym, mods)) handled = true;
        }
    }

    if (handled) {
        swallow_add(ev->keycode);
    } else {
        if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED
            && !(nsyms > 0 && is_modifier_sym(syms[0]))) {
            cursor_hide_now();
        }

        wlr_seat_keyboard_notify_key(
            server.seat, ev->time_msec, ev->keycode, ev->state
        );
    }
}

static void keyboard_handle_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct Keyboard *kb = wl_container_of(l, kb, destroy);
    wl_list_remove(&kb->modifiers.link);
    wl_list_remove(&kb->key.link);
    wl_list_remove(&kb->destroy.link);
    wl_list_remove(&kb->link);
    free(kb);
}

void seat_handle_new_input(struct wlr_input_device *device) {
    struct DeviceEntry *entry = calloc(1, sizeof(*entry));
    if (entry == NULL) return;

    entry->device = device;
    entry->destroy.notify = device_entry_destroy;

    wl_signal_add(&device->events.destroy, &entry->destroy);
    wl_list_insert(&input_devices, &entry->link);

    switch (device->type) {
        case WLR_INPUT_DEVICE_KEYBOARD:
            {
                struct Keyboard *kb = calloc(1, sizeof(*kb));
                if (kb == NULL) break;
                kb->wlr_keyboard = wlr_keyboard_from_input_device(device);

                keymap_apply(kb->wlr_keyboard);

                kb->modifiers.notify = keyboard_handle_modifiers;
                wl_signal_add(
                    &kb->wlr_keyboard->events.modifiers, &kb->modifiers
                );

                kb->key.notify = keyboard_handle_key;
                wl_signal_add(&kb->wlr_keyboard->events.key, &kb->key);

                kb->destroy.notify = keyboard_handle_destroy;
                wl_signal_add(&device->events.destroy, &kb->destroy);

                wl_list_insert(&server.keyboards, &kb->link);
                wlr_seat_set_keyboard(server.seat, kb->wlr_keyboard);

                break;
            }

        case WLR_INPUT_DEVICE_POINTER:
            wlr_cursor_attach_input_device(server.cursor, device);
            input_apply_pointer_config(device);
            break;

        case WLR_INPUT_DEVICE_TOUCH:
            wlr_cursor_attach_input_device(server.cursor, device);
            break;

        case WLR_INPUT_DEVICE_TABLET:
            wlr_cursor_attach_input_device(server.cursor, device);
            break;

        default: break;
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server.keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server.seat, caps);
}

static void seat_update_decorations(const struct Window *focused) {
    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->closed) continue;
        deco_update_active(w, w == focused); // no-op without a decoration
    }
}

static struct Window *seat_window_for_surface(const struct wlr_surface *surface
) {
    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (!w->closed && window_wlr_surface(w) == surface) return w;
    }

    return NULL;
}

void seat_focus(struct Window *window) {
    if (server.locked) {
        if (window != NULL) {
            wl_list_remove(&window->focus_link);
            wl_list_insert(wm.focus_stack.prev, &window->focus_link);
        }

        return;
    }

    if (window == NULL) window = wm_focus_stack_top();

    struct wlr_surface *want = window ? window_wlr_surface(window) : NULL;
    if (window != NULL && want == NULL) return;

    struct wlr_surface *prev = server.seat->keyboard_state.focused_surface;
    if (prev == want) {
        seat_update_decorations(window);
        return;
    }

    if (prev != NULL) {
        struct Window *pw = seat_window_for_surface(prev);
        if (pw != NULL) window_set_activated(pw, false);
    }

    if (window != NULL) {
        window_set_activated(window, true);

        struct wlr_keyboard *kb = wlr_seat_get_keyboard(server.seat);
        wlr_seat_keyboard_notify_enter(
            server.seat, want, kb ? kb->keycodes : NULL,
            kb ? kb->num_keycodes : 0, kb ? &kb->modifiers : NULL
        );

        wl_list_remove(&window->focus_link);
        wl_list_insert(wm.focus_stack.prev, &window->focus_link);
    } else {
        wlr_seat_keyboard_clear_focus(server.seat);
    }

    if (resize_mode.active && resize_mode.window != window) {
        resize_mode_exit();
    }

    seat_update_decorations(window);
    ipc_notify_focus();
    wm_mark_layout_dirty();
}
