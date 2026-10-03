#ifndef INPUT_H
#define INPUT_H

#include "actions.h"

#include <stddef.h>
#include <stdint.h>
#include <wayland-client-core.h>
#include <wayland-util.h>
#include <xkbcommon/xkbcommon.h>

struct Seat;

struct KeyDef {
    uint32_t mods;
    xkb_keysym_t key;
    enum Action action;
    const void *arg;
};

struct PointerDef {
    uint32_t mods;
    uint32_t button;
    enum Action action;
    const void *arg;
};

struct XkbBinding {
    struct river_xkb_binding_v1 *obj;
    struct Seat *seat;
    struct wl_list link;
    enum Action action;
    const void *arg;
};

struct PointerBinding {
    struct river_pointer_binding_v1 *obj;
    struct Seat *seat;
    struct wl_list link;
    enum Action action;
    const void *arg;
};

struct LibinputConfig {
    int tap_state;
    int natural_scroll;
    int left_handed;
    int middle_emulation;
    int dwt;
    int drag;
    int drag_lock;
    int three_finger_drag;
    int accel_profile;
    float accel_speed;
    int click_method;
    int scroll_method;
};

extern const struct KeyDef cfg_keybinds[];
extern const size_t cfg_keybinds_len;
extern const struct PointerDef cfg_pointer_binds[];
extern const size_t cfg_pointer_binds_len;

// Create all configured xkb + pointer bindings for a new seat.
void input_seat_bind_all(struct Seat *seat);

// Destroy every binding owned by the seat.
void input_seat_unbind_all(struct Seat *seat);

struct wl_registry;

void libinput_init(void);
void libinput_bind(struct wl_registry *registry, uint32_t name);
void libinput_reconfigure(void);

#endif // INPUT_H
