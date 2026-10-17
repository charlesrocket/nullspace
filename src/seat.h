#pragma once

#include "input.h"

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>

struct Window;

struct Keyboard {
    struct wlr_keyboard *wlr_keyboard;
    struct wl_list link; // NsServer.keyboards
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

void seat_init(void);
void seat_handle_new_input(struct wlr_input_device *device);
void seat_foreach_input_device(
    void (*fn)(struct wlr_input_device *, void *), void *data
);

void seat_focus(struct Window *window);
