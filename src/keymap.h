#ifndef KEYMAP_H
#define KEYMAP_H

#include <stdint.h>
#include <wayland-client-protocol.h>

typedef void (*KeymapLayoutCallback)(const char *layout);

void keymap_set_layout_callback(KeymapLayoutCallback cb);

void keymap_init(void);
void keymap_bind(struct wl_registry *registry, uint32_t name);
void keymap_destroy(void);

#endif // KEYMAP_H
