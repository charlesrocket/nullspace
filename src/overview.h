#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

struct wlr_pointer_button_event;
struct Window;

void overview_enter(void);
void overview_exit(void);
void overview_layout(void);
void overview_window_destroy(struct Window* window);

bool overview_handle_button(struct wlr_pointer_button_event* ev);
bool overview_handle_motion(double lx, double ly);
bool overview_tick(void);
bool overview_is_animating(void);

bool overview_handle_key(
    uint32_t keycode, const xkb_keysym_t* syms, int nsyms, uint32_t mods
);
