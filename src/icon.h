#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct wl_display;

struct IconBuffer {
    int32_t scale;
    int32_t size;    // width == height
    uint32_t format; // DRM_FORMAT_*
    uint8_t* pixels; // owned, size*size*4
};

struct Icon {
    char* name;
    struct IconBuffer* buffers;
    size_t buffer_count;
};

bool icon_manager_init(struct wl_display* display);
void icon_manager_finish(void);
void icon_free(struct Icon* ic);
struct Icon* icon_clone(const struct Icon* src);
