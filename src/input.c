#include "input.h"

#include "config.h"
#include "nullspace.h"

#include <dev/evdev/input-event-codes.h>
#include <river-libinput-config-v1-client-protocol.h>
#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <wayland-util.h>
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

struct LibinputDevice {
    struct river_libinput_device_v1 *obj;
    struct wl_list link;
};

static struct river_libinput_config_v1 *libinput_config;
static struct wl_list devices; // LibinputDevice

void libinput_init(void) { wl_list_init(&devices); }

static void device_handle_tap_support(
    void *data, struct river_libinput_device_v1 *obj, int32_t finger_count
) {
    if (wm.libinput.tap_state < 0) { return; }
    if (finger_count < 1) { return; }

    river_libinput_device_v1_set_tap(obj, (uint32_t)wm.libinput.tap_state);
}

static void device_handle_natural_scroll_support(
    void *data, struct river_libinput_device_v1 *obj, int32_t supported
) {
    if (wm.libinput.natural_scroll < 0) { return; }
    if (!supported) { return; }

    river_libinput_device_v1_set_natural_scroll(
        obj, (uint32_t)wm.libinput.natural_scroll
    );
}

static void device_handle_left_handed_support(
    void *data, struct river_libinput_device_v1 *obj, int32_t supported
) {
    if (wm.libinput.left_handed < 0) { return; }
    if (!supported) { return; }

    river_libinput_device_v1_set_left_handed(
        obj, (uint32_t)wm.libinput.left_handed
    );
}

static void device_handle_middle_emulation_support(
    void *data, struct river_libinput_device_v1 *obj, int32_t supported
) {
    if (wm.libinput.middle_emulation < 0) { return; }
    if (!supported) { return; }

    river_libinput_device_v1_set_middle_emulation(
        obj, (uint32_t)wm.libinput.middle_emulation
    );
}

static void device_handle_dwt_support(
    void *data, struct river_libinput_device_v1 *obj, int32_t supported
) {
    if (wm.libinput.dwt < 0) { return; }
    if (!supported) { return; }

    river_libinput_device_v1_set_dwt(obj, (uint32_t)wm.libinput.dwt);
}

static void device_handle_drag_default(
    void *data, struct river_libinput_device_v1 *obj, uint32_t state
) {
    if (wm.libinput.drag < 0) { return; }

    river_libinput_device_v1_set_drag(obj, (uint32_t)wm.libinput.drag);
}

static void device_handle_drag_lock_default(
    void *data, struct river_libinput_device_v1 *obj, uint32_t state
) {
    if (wm.libinput.drag_lock < 0) { return; }

    river_libinput_device_v1_set_drag_lock(
        obj, (uint32_t)wm.libinput.drag_lock
    );
}

static void device_handle_three_finger_drag_support(
    void *data, struct river_libinput_device_v1 *obj, int32_t finger_count
) {
    if (wm.libinput.three_finger_drag < 0) { return; }
    if (finger_count < 3) { return; }

    river_libinput_device_v1_set_three_finger_drag(
        obj, (uint32_t)wm.libinput.three_finger_drag
    );
}

static void device_handle_accel_profiles_support(
    void *data, struct river_libinput_device_v1 *obj, uint32_t profiles
) {
    if (wm.libinput.accel_profile < 0) { return; }

    uint32_t profile = (uint32_t)wm.libinput.accel_profile;
    if ((profiles & profile) == 0) { return; }

    river_libinput_device_v1_set_accel_profile(obj, profile);
}

static void device_handle_accel_speed_default(
    void *data, struct river_libinput_device_v1 *obj, struct wl_array *speed
) {
    if (wm.libinput.accel_speed <= -2.0f) { return; }

    struct wl_array buf;
    wl_array_init(&buf);

    double *slot = wl_array_add(&buf, sizeof(*slot));
    if (slot != NULL) {
        *slot = (double)wm.libinput.accel_speed;
        river_libinput_device_v1_set_accel_speed(obj, &buf);
    }

    wl_array_release(&buf);
}

static void device_handle_click_method_support(
    void *data, struct river_libinput_device_v1 *obj, uint32_t methods
) {
    if (wm.libinput.click_method < 0) { return; }

    uint32_t method = (uint32_t)wm.libinput.click_method;
    if ((methods & method) == 0) { return; }

    river_libinput_device_v1_set_click_method(obj, method);
}

static void device_handle_scroll_method_support(
    void *data, struct river_libinput_device_v1 *obj, uint32_t methods
) {
    if (wm.libinput.scroll_method < 0) { return; }

    uint32_t method = (uint32_t)wm.libinput.scroll_method;
    if ((methods & method) == 0) { return; }

    river_libinput_device_v1_set_scroll_method(obj, method);
}

static void
device_handle_removed(void *data, struct river_libinput_device_v1 *obj) {
    struct LibinputDevice *dev = data;

    river_libinput_device_v1_destroy(dev->obj);
    wl_list_remove(&dev->link);
    free(dev);
}

static void device_ignore_i32(
    void *data, struct river_libinput_device_v1 *obj, int32_t value
) {}

static void device_ignore_u32(
    void *data, struct river_libinput_device_v1 *obj, uint32_t value
) {}

static void device_ignore_array(
    void *data, struct river_libinput_device_v1 *obj, struct wl_array *value
) {}

static void device_ignore_object(
    void *data, struct river_libinput_device_v1 *obj,
    struct river_input_device_v1 *value
) {}

static void
device_handle_done(void *data, struct river_libinput_device_v1 *obj) {}

static const struct river_libinput_device_v1_listener device_listener = {
    .removed = device_handle_removed,
    .input_device = device_ignore_object,

    .send_events_support = device_ignore_u32,
    .send_events_default = device_ignore_u32,
    .send_events_current = device_ignore_u32,

    .tap_support = device_handle_tap_support,
    .tap_default = device_ignore_u32,
    .tap_current = device_ignore_u32,
    .tap_button_map_default = device_ignore_u32,
    .tap_button_map_current = device_ignore_u32,

    .drag_default = device_handle_drag_default,
    .drag_current = device_ignore_u32,
    .drag_lock_default = device_handle_drag_lock_default,
    .drag_lock_current = device_ignore_u32,

    .three_finger_drag_support = device_handle_three_finger_drag_support,
    .three_finger_drag_default = device_ignore_u32,
    .three_finger_drag_current = device_ignore_u32,

    .calibration_matrix_support = device_ignore_i32,
    .calibration_matrix_default = device_ignore_array,
    .calibration_matrix_current = device_ignore_array,

    .accel_profiles_support = device_handle_accel_profiles_support,
    .accel_profile_default = device_ignore_u32,
    .accel_profile_current = device_ignore_u32,
    .accel_speed_default = device_handle_accel_speed_default,
    .accel_speed_current = device_ignore_array,

    .natural_scroll_support = device_handle_natural_scroll_support,
    .natural_scroll_default = device_ignore_u32,
    .natural_scroll_current = device_ignore_u32,

    .left_handed_support = device_handle_left_handed_support,
    .left_handed_default = device_ignore_u32,
    .left_handed_current = device_ignore_u32,

    .click_method_support = device_handle_click_method_support,
    .click_method_default = device_ignore_u32,
    .click_method_current = device_ignore_u32,
    .clickfinger_button_map_default = device_ignore_u32,
    .clickfinger_button_map_current = device_ignore_u32,

    .middle_emulation_support = device_handle_middle_emulation_support,
    .middle_emulation_default = device_ignore_u32,
    .middle_emulation_current = device_ignore_u32,

    .scroll_method_support = device_handle_scroll_method_support,
    .scroll_method_default = device_ignore_u32,
    .scroll_method_current = device_ignore_u32,
    .scroll_button_default = device_ignore_u32,
    .scroll_button_current = device_ignore_u32,
    .scroll_button_lock_default = device_ignore_u32,
    .scroll_button_lock_current = device_ignore_u32,

    .dwt_support = device_handle_dwt_support,
    .dwt_default = device_ignore_u32,
    .dwt_current = device_ignore_u32,
    .dwtp_support = device_ignore_i32,
    .dwtp_default = device_ignore_u32,
    .dwtp_current = device_ignore_u32,

    .rotation_support = device_ignore_i32,
    .rotation_default = device_ignore_u32,
    .rotation_current = device_ignore_u32,

    .done = device_handle_done,
};

static void
config_handle_finished(void *data, struct river_libinput_config_v1 *obj) {
    struct LibinputDevice *dev, *tmp;
    wl_list_for_each_safe(dev, tmp, &devices, link) {
        wl_list_remove(&dev->link);
        river_libinput_device_v1_destroy(dev->obj);
        free(dev);
    }

    river_libinput_config_v1_destroy(obj);
    libinput_config = NULL;
}

static void config_handle_libinput_device(
    void *data, struct river_libinput_config_v1 *obj,
    struct river_libinput_device_v1 *id
) {
    struct LibinputDevice *dev = calloc(1, sizeof(struct LibinputDevice));
    if (dev == NULL) { return; }

    dev->obj = id;
    wl_list_insert(devices.prev, &dev->link);

    river_libinput_device_v1_add_listener(id, &device_listener, dev);
}

static const struct river_libinput_config_v1_listener config_listener = {
    .finished = config_handle_finished,
    .libinput_device = config_handle_libinput_device,
};

void libinput_bind(struct wl_registry *registry, uint32_t name) {
    if (libinput_config != NULL) { return; }

    struct river_libinput_config_v1 *config = wl_registry_bind(
        registry, name, &river_libinput_config_v1_interface, 2
    );

    if (config == NULL) { return; }

    libinput_config = config;
    river_libinput_config_v1_add_listener(config, &config_listener, NULL);
}
