#include "input.h"

#include "config.h"
#include "nullspace.h"

#include <dev/evdev/input-event-codes.h>
#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>
#include <stdlib.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define SUPER RIVER_SEAT_V1_MODIFIERS_MOD4
#define SHIFT RIVER_SEAT_V1_MODIFIERS_SHIFT

static const char *const cmd_terminal[] = CFG_TERMINAL;

#define SPACEKEY(KEY, N)                                                       \
    {SUPER, KEY, ACTION_SPACE_##N, NULL},                                      \
        {SUPER | SHIFT, KEY, ACTION_MOVE_TO_SPACE_##N, NULL},

const struct KeyDef cfg_keybinds[] = {
    {SUPER, XKB_KEY_Return,        ACTION_SPAWN, cmd_terminal},
    {SUPER,      XKB_KEY_q,        ACTION_CLOSE,         NULL},
    {SUPER,      XKB_KEY_f,   ACTION_FOCUS_NEXT,         NULL},
    {SUPER,      XKB_KEY_l, ACTION_CYCLE_LAYOUT,         NULL},
    {SUPER,      XKB_KEY_r,         ACTION_EXIT,         NULL},

    SPACEKEY(XKB_KEY_1, 1) SPACEKEY(XKB_KEY_2, 2) SPACEKEY(XKB_KEY_3, 3)
        SPACEKEY(XKB_KEY_4, 4) SPACEKEY(XKB_KEY_5, 5) SPACEKEY(XKB_KEY_6, 6)
            SPACEKEY(XKB_KEY_7, 7) SPACEKEY(XKB_KEY_8, 8) SPACEKEY(XKB_KEY_9, 9)
                SPACEKEY(XKB_KEY_0, 10)
};

const size_t cfg_keybinds_len = sizeof(cfg_keybinds) / sizeof(cfg_keybinds[0]);

const struct PointerDef cfg_pointer_binds[] = {
    {SUPER,  BTN_LEFT,   ACTION_MOVE, NULL},
    {SUPER, BTN_RIGHT, ACTION_RESIZE, NULL},
};

const size_t cfg_pointer_binds_len =
    sizeof(cfg_pointer_binds) / sizeof(cfg_pointer_binds[0]);

extern struct river_xkb_bindings_v1 *xkb_bindings_v1;

static void
xkb_binding_handle_pressed(void *data, struct river_xkb_binding_v1 *obj) {
    struct XkbBinding *binding = data;
    binding->seat->pending_action = binding->action;
    binding->seat->pending_arg = binding->arg;
}

static void
xkb_binding_handle_released(void *data, struct river_xkb_binding_v1 *obj) {}

static const struct river_xkb_binding_v1_listener xkb_binding_listener = {
    .pressed = xkb_binding_handle_pressed,
    .released = xkb_binding_handle_released,
};

static void xkb_binding_destroy(struct XkbBinding *binding) {
    river_xkb_binding_v1_destroy(binding->obj);
    wl_list_remove(&binding->link);
    free(binding);
}

static void xkb_binding_create(struct Seat *seat, const struct KeyDef *def) {
    struct XkbBinding *binding = calloc(1, sizeof(struct XkbBinding));
    if (binding == NULL) { return; }

    binding->obj = river_xkb_bindings_v1_get_xkb_binding(
        xkb_bindings_v1, seat->obj, def->key, def->mods
    );

    binding->seat = seat;
    binding->action = def->action;
    binding->arg = def->arg;

    river_xkb_binding_v1_add_listener(
        binding->obj, &xkb_binding_listener, binding
    );

    river_xkb_binding_v1_enable(binding->obj);

    wl_list_insert(seat->xkb_bindings.prev, &binding->link);
}

static void pointer_binding_handle_pressed(
    void *data, struct river_pointer_binding_v1 *obj
) {
    struct PointerBinding *binding = data;
    binding->seat->pending_action = binding->action;
    binding->seat->pending_arg = binding->arg;
}

static void pointer_binding_handle_released(
    void *data, struct river_pointer_binding_v1 *obj
) {}

static const struct river_pointer_binding_v1_listener pointer_binding_listener =
    {
        .pressed = pointer_binding_handle_pressed,
        .released = pointer_binding_handle_released,
};

static void pointer_binding_destroy(struct PointerBinding *binding) {
    river_pointer_binding_v1_destroy(binding->obj);
    wl_list_remove(&binding->link);
    free(binding);
}

static void
pointer_binding_create(struct Seat *seat, const struct PointerDef *def) {
    struct PointerBinding *binding = calloc(1, sizeof(struct PointerBinding));
    if (binding == NULL) { return; }

    binding->obj =
        river_seat_v1_get_pointer_binding(seat->obj, def->button, def->mods);

    binding->seat = seat;
    binding->action = def->action;
    binding->arg = def->arg;

    river_pointer_binding_v1_add_listener(
        binding->obj, &pointer_binding_listener, binding
    );

    river_pointer_binding_v1_enable(binding->obj);

    wl_list_insert(seat->pointer_bindings.prev, &binding->link);
}

void input_seat_bind_all(struct Seat *seat) {
    for (size_t i = 0; i < cfg_keybinds_len; i++) {
        xkb_binding_create(seat, &cfg_keybinds[i]);
    }

    for (size_t i = 0; i < cfg_pointer_binds_len; i++) {
        pointer_binding_create(seat, &cfg_pointer_binds[i]);
    }
}

void input_seat_unbind_all(struct Seat *seat) {
    struct XkbBinding *xkb, *xkb_tmp;
    wl_list_for_each_safe(xkb, xkb_tmp, &seat->xkb_bindings, link) {
        xkb_binding_destroy(xkb);
    }

    struct PointerBinding *ptr, *ptr_tmp;
    wl_list_for_each_safe(ptr, ptr_tmp, &seat->pointer_bindings, link) {
        pointer_binding_destroy(ptr);
    }
}
