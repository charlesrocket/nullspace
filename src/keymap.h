#pragma once

#include <stdint.h>

struct wlr_keyboard;

typedef void (*KeymapLayoutCallback)(const char *layout);

void keymap_set_layout_callback(KeymapLayoutCallback cb);

void keymap_init(void);
void keymap_destroy(void);
void keymap_apply(struct wlr_keyboard *keyboard);
void keymap_notify_layout(struct wlr_keyboard *keyboard);
