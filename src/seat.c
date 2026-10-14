#include "seat.h"

#include "include/layouts.h"
#include "input.h"
#include "ipc.h"
#include "keymap.h"
#include "layouts/layout.h"
#include "manager.h"
#include "server.h"
#include "src/actions.h"
#include "view.h"

#include <dev/evdev/input-event-codes.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#define LOG_TOPIC "seat"

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

void seat_init(void) { wl_list_init(&input_devices); }

static void seat_action(enum Action action, const void *arg) {
    switch (action) {
        case ACTION_NONE: break;

        case ACTION_SPAWN:
            {
                const char *const *argv = arg;
                if (argv == NULL || argv[0] == NULL) break;

                pid_t pid = fork();
                if (pid == 0) {
                    setsid();

                    int devnull = open("/dev/null", O_RDWR);
                    if (devnull >= 0) {
                        dup2(devnull, STDIN_FILENO);
                        dup2(devnull, STDOUT_FILENO);
                        dup2(devnull, STDERR_FILENO);
                        if (devnull > STDERR_FILENO) close(devnull);
                    }

                    execvp(argv[0], (char *const *)argv);
                    _exit(127);
                }

                break;
            }

        case ACTION_CLOSE:
            {
                struct Window *focused = view_focused();
                if (focused != NULL) {
                    struct Window *next = focus_stack_top_except(focused);
                    view_close(focused);

                    if (next != NULL) seat_focus(next);
                }

                break;
            }

        case ACTION_FOCUS_NEXT:
            {
                struct Window *next = window_next(view_focused());
                if (next != NULL) seat_focus(next);
                break;
            }

        case ACTION_MOVE:
        case ACTION_RESIZE: break;
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
            wm_move_window_to_space(view_focused(), 0);
            break;
        case ACTION_MOVE_TO_SPACE_1:
            wm_move_window_to_space(view_focused(), 1);
            break;
        case ACTION_MOVE_TO_SPACE_2:
            wm_move_window_to_space(view_focused(), 2);
            break;
        case ACTION_MOVE_TO_SPACE_3:
            wm_move_window_to_space(view_focused(), 3);
            break;
        case ACTION_MOVE_TO_SPACE_4:
            wm_move_window_to_space(view_focused(), 4);
            break;
        case ACTION_MOVE_TO_SPACE_5:
            wm_move_window_to_space(view_focused(), 5);
            break;
        case ACTION_MOVE_TO_SPACE_6:
            wm_move_window_to_space(view_focused(), 6);
            break;
        case ACTION_MOVE_TO_SPACE_7:
            wm_move_window_to_space(view_focused(), 7);
            break;
        case ACTION_MOVE_TO_SPACE_8:
            wm_move_window_to_space(view_focused(), 8);
            break;
        case ACTION_MOVE_TO_SPACE_9:
            wm_move_window_to_space(view_focused(), 9);
            break;
    }
}

static bool handle_keybinding(xkb_keysym_t sym, uint32_t mods) {
    for (size_t i = 0; i < cfg_keybinds_len; i++) {
        const struct KeyDef *kb = &cfg_keybinds[i];
        if (kb->key != sym) continue;
        if (kb->mods != mods) continue;

        seat_action(kb->action, kb->arg);
        return true;
    }

    return false;
}

static void keyboard_handle_modifiers(struct wl_listener *l, void *data) {
    (void)data;

    struct Keyboard *kb = wl_container_of(l, kb, modifiers);
    wlr_seat_set_keyboard(server.seat, kb->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(
        server.seat, &kb->wlr_keyboard->modifiers
    );

    keymap_notify_layout(kb->wlr_keyboard);
}

static void keyboard_handle_key(struct wl_listener *l, void *data) {
    struct Keyboard *kb = wl_container_of(l, kb, key);
    struct wlr_keyboard_key_event *ev = data;

    wlr_seat_set_keyboard(server.seat, kb->wlr_keyboard);

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

    uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);

    bool handled = false;

    // Only match keybinds on PRESS. The key event fires on both press and
    // release, and the keysym + modifiers are identical in both cases, (without
    // this check every binding runs twice per keystroke).
    if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            if (handle_keybinding(syms[i], mods)) handled = true;
        }

        if (!handled && layout_sym != XKB_KEY_NoSymbol) {
            if (handle_keybinding(layout_sym, mods)) handled = true;
        }
    }

    if (!handled) {
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

        default: wlr_cursor_attach_input_device(server.cursor, device); break;
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server.keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server.seat, caps);
}

void seat_focus(struct Window *window) {
    if (window == NULL) window = focus_stack_top();

    struct wlr_surface *want =
        window ? window->xdg_toplevel->base->surface : NULL;

    struct wlr_surface *prev = server.seat->keyboard_state.focused_surface;

    if (prev == want) return;
    if (prev != NULL) {
        struct wlr_xdg_toplevel *old =
            wlr_xdg_toplevel_try_from_wlr_surface(prev);

        if (old != NULL) wlr_xdg_toplevel_set_activated(old, false);
    }

    if (window != NULL) {
        wlr_xdg_toplevel_set_activated(window->xdg_toplevel, true);
        wlr_seat_keyboard_notify_enter(server.seat, want, NULL, 0, NULL);
        wl_list_remove(&window->focus_link);
        wl_list_insert(wm.focus_stack.prev, &window->focus_link);
    } else {
        wlr_seat_keyboard_clear_focus(server.seat);
    }

    ipc_notify_focus();
    wm_mark_layout_dirty();
}
