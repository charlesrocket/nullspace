#include "input.h"

#include "config.h"
#include "manager.h"
#include "seat.h"
#include "server.h"

#include <dev/evdev/input-event-codes.h>
#include <libinput.h>
#include <stdint.h>
#include <stdlib.h>
#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_input_device.h>

#define LOG_TOPIC "input"

const struct KeyDef cfg_keybinds[] = {CFG_KEYBINDS};
const size_t cfg_keybinds_len = sizeof(cfg_keybinds) / sizeof(cfg_keybinds[0]);

const struct PointerDef cfg_pointer_binds[] = {CFG_POINTER_BINDS};
const size_t cfg_pointer_binds_len =
    sizeof(cfg_pointer_binds) / sizeof(cfg_pointer_binds[0]);

static struct libinput_device *libinput_of(struct wlr_input_device *device) {
    if (!wlr_input_device_is_libinput(device)) return NULL;
    return wlr_libinput_get_device_handle(device);
}

static void apply_tap(struct libinput_device *dev) {
    if (wm.libinput.tap_state < 0) return;
    if (libinput_device_config_tap_get_finger_count(dev) < 1) return;

    libinput_device_config_tap_set_enabled(
        dev, wm.libinput.tap_state ? LIBINPUT_CONFIG_TAP_ENABLED
                                   : LIBINPUT_CONFIG_TAP_DISABLED
    );
}

static void apply_natural_scroll(struct libinput_device *dev) {
    if (wm.libinput.natural_scroll < 0) return;
    if (!libinput_device_config_scroll_has_natural_scroll(dev)) return;

    libinput_device_config_scroll_set_natural_scroll_enabled(
        dev, wm.libinput.natural_scroll ? 1 : 0
    );
}

static void apply_left_handed(struct libinput_device *dev) {
    if (wm.libinput.left_handed < 0) return;
    if (!libinput_device_config_left_handed_is_available(dev)) return;

    libinput_device_config_left_handed_set(
        dev, wm.libinput.left_handed ? 1 : 0
    );
}

static void apply_middle_emulation(struct libinput_device *dev) {
    if (wm.libinput.middle_emulation < 0) return;
    if (!libinput_device_config_middle_emulation_is_available(dev)) return;

    libinput_device_config_middle_emulation_set_enabled(
        dev, wm.libinput.middle_emulation
                 ? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED
                 : LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED
    );
}

static void apply_dwt(struct libinput_device *dev) {
    if (wm.libinput.dwt < 0) return;
    if (!libinput_device_config_dwt_is_available(dev)) return;

    libinput_device_config_dwt_set_enabled(
        dev, wm.libinput.dwt ? LIBINPUT_CONFIG_DWT_ENABLED
                             : LIBINPUT_CONFIG_DWT_DISABLED
    );
}

static void apply_drag(struct libinput_device *dev) {
    if (wm.libinput.drag < 0) return;
    if (libinput_device_config_tap_get_finger_count(dev) < 1) return;

    libinput_device_config_tap_set_drag_enabled(
        dev, wm.libinput.drag ? LIBINPUT_CONFIG_DRAG_ENABLED
                              : LIBINPUT_CONFIG_DRAG_DISABLED
    );
}

static void apply_drag_lock(struct libinput_device *dev) {
    if (wm.libinput.drag_lock < 0) return;
    if (libinput_device_config_tap_get_finger_count(dev) < 1) return;

    enum libinput_config_drag_lock_state state =
        LIBINPUT_CONFIG_DRAG_LOCK_DISABLED;

    if (wm.libinput.drag_lock == 1) {
        state = LIBINPUT_CONFIG_DRAG_LOCK_ENABLED_TIMEOUT;
    } else if (wm.libinput.drag_lock == 2) {
        state = LIBINPUT_CONFIG_DRAG_LOCK_ENABLED_STICKY;
    }

    libinput_device_config_tap_set_drag_lock_enabled(dev, state);
}

static void apply_three_finger_drag(struct libinput_device *dev) {
    if (wm.libinput.three_finger_drag < 0) return;
    if (libinput_device_config_tap_get_finger_count(dev) < 3) return;

    libinput_device_config_3fg_drag_set_enabled(
        dev, wm.libinput.three_finger_drag
                 ? LIBINPUT_CONFIG_3FG_DRAG_ENABLED_3FG
                 : LIBINPUT_CONFIG_3FG_DRAG_DISABLED
    );
}

static void apply_accel(struct libinput_device *dev) {
    if (!libinput_device_config_accel_is_available(dev)) return;

    if (wm.libinput.accel_profile >= 0) {
        uint32_t supported = libinput_device_config_accel_get_profiles(dev);
        uint32_t want = (uint32_t)wm.libinput.accel_profile;

        enum libinput_config_accel_profile prof =
            LIBINPUT_CONFIG_ACCEL_PROFILE_NONE;

        if (want == 1) {
            prof = LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT;
        } else if (want == 2) {
            prof = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
        } else if (want == 4) {
            prof = LIBINPUT_CONFIG_ACCEL_PROFILE_CUSTOM;
        }

        if (want == 0 || (supported & want) != 0) {
            libinput_device_config_accel_set_profile(dev, prof);
        }
    }

    if (wm.libinput.accel_speed > -2.0f) {
        libinput_device_config_accel_set_speed(
            dev, (double)wm.libinput.accel_speed
        );
    }
}

static void apply_click_method(struct libinput_device *dev) {
    if (wm.libinput.click_method < 0) return;

    uint32_t methods = libinput_device_config_click_get_methods(dev);
    if (methods == 0) return;

    enum libinput_config_click_method m = LIBINPUT_CONFIG_CLICK_METHOD_NONE;

    if (wm.libinput.click_method == 1) {
        m = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
    } else if (wm.libinput.click_method == 2) {
        m = LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER;
    }

    if (m != LIBINPUT_CONFIG_CLICK_METHOD_NONE
        && (methods & (uint32_t)m) == 0) {
        m = LIBINPUT_CONFIG_CLICK_METHOD_NONE;
    }

    libinput_device_config_click_set_method(dev, m);
}

static void apply_scroll_method(struct libinput_device *dev) {
    if (wm.libinput.scroll_method < 0) return;

    uint32_t methods = libinput_device_config_scroll_get_methods(dev);
    if (methods == 0) return;

    enum libinput_config_scroll_method m = LIBINPUT_CONFIG_SCROLL_NO_SCROLL;

    if (wm.libinput.scroll_method == 1) {
        m = LIBINPUT_CONFIG_SCROLL_2FG;
    } else if (wm.libinput.scroll_method == 2) {
        m = LIBINPUT_CONFIG_SCROLL_EDGE;
    } else if (wm.libinput.scroll_method == 4) {
        m = LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN;
    }

    if (m != LIBINPUT_CONFIG_SCROLL_NO_SCROLL && (methods & (uint32_t)m) == 0) {
        m = LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
    }

    libinput_device_config_scroll_set_method(dev, m);
}

static void apply_all(struct libinput_device *dev) {
    apply_tap(dev);
    apply_natural_scroll(dev);
    apply_left_handed(dev);
    apply_middle_emulation(dev);
    apply_dwt(dev);
    apply_drag(dev);
    apply_drag_lock(dev);
    apply_three_finger_drag(dev);
    apply_accel(dev);
    apply_click_method(dev);
    apply_scroll_method(dev);
}

void input_apply_pointer_config(struct wlr_input_device *device) {
    struct libinput_device *dev = libinput_of(device);
    if (dev == NULL) return;
    apply_all(dev);
}

static void reconfigure_visit(struct wlr_input_device *device, void *data) {
    (void)data;

    struct libinput_device *dev = libinput_of(device);
    if (dev == NULL) return;
    apply_all(dev);
}

void libinput_reconfigure(void) {
    seat_foreach_input_device(reconfigure_visit, NULL);
}
