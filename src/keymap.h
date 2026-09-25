#ifndef KEYMAP_H
#define KEYMAP_H

#include <stdint.h>
#include <wayland-client-protocol.h>

void keymap_set_layout(const char *layout);
void keymap_init(void);
void keymap_bind(struct wl_registry *registry, uint32_t name);
void keymap_destroy(void);

#endif // KEYMAP_H
