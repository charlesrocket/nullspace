#pragma once

#include "actions.h"

#include <stddef.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

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

struct LibinputConfig {
    float accel_speed;
    int tap_state;
    int natural_scroll;
    int left_handed;
    int middle_emulation;
    int dwt;
    int drag;
    int drag_lock;
    int three_finger_drag;
    int accel_profile;
    int click_method;
    int scroll_method;
};

extern const struct KeyDef cfg_keybinds[];
extern const size_t cfg_keybinds_len;
extern const struct PointerDef cfg_pointer_binds[];
extern const size_t cfg_pointer_binds_len;

struct wlr_input_device;

void input_apply_pointer_config(struct wlr_input_device *device);
void libinput_reconfigure(void);
