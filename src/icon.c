#include "icon.h"

#include "log.h"
#include "window.h"

#include <drm_fourcc.h>
#include <scenefx/types/wlr_scene.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_toplevel_icon_v1.h>

#define LOG_TOPIC            "icon"

#define ICON_BYTES_PER_PIXEL 4u
#define ICON_MAX_SIZE        256 // px per side, per buffer
#define ICON_MAX_BUFFERS     8   // per icon

static struct wlr_xdg_toplevel_icon_manager_v1 *manager;
static struct wl_listener set_icon_listener;

void icon_free(struct Icon *ic) {
    if (ic == NULL) return;

    for (size_t i = 0; i < ic->buffer_count; i++) free(ic->buffers[i].pixels);
    free(ic->buffers);
    free(ic->name);
    *ic = (struct Icon){0};
}

static void icon_destroy(struct Icon *ic) {
    icon_free(ic);
    free(ic);
}

static struct Icon *icon_new(const char *name, size_t capacity) {
    struct Icon *ic = calloc(1, sizeof(*ic));
    if (ic == NULL) return NULL;

    ic->name = name != NULL ? strdup(name) : NULL;
    ic->buffers = capacity > 0 ? calloc(capacity, sizeof(*ic->buffers)) : NULL;

    if ((name != NULL && ic->name == NULL)
        || (capacity > 0 && ic->buffers == NULL)) {
        icon_destroy(ic);
        return NULL;
    }

    return ic;
}

static size_t buffer_bytes(int32_t size) {
    return (size_t)size * (size_t)size * ICON_BYTES_PER_PIXEL;
}

static bool
icon_buffer_clone(struct IconBuffer *dst, const struct IconBuffer *src) {
    size_t bytes = buffer_bytes(src->size);
    uint8_t *pixels = malloc(bytes);
    if (pixels == NULL) return false;

    memcpy(pixels, src->pixels, bytes);
    *dst = *src;
    dst->pixels = pixels;
    return true;
}

struct Icon *icon_clone(const struct Icon *src) {
    if (src == NULL) return NULL;

    struct Icon *dst = icon_new(src->name, src->buffer_count);
    if (dst == NULL) return NULL;

    for (size_t i = 0; i < src->buffer_count; i++) {
        if (!icon_buffer_clone(&dst->buffers[i], &src->buffers[i])) {
            icon_destroy(dst);
            return NULL;
        }
        dst->buffer_count++;
    }

    return dst;
}

static bool format_supported(uint32_t fmt) {
    switch (fmt) {
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_XBGR8888: return true;
        default: return false;
    }
}

static bool
snapshot_buffer(struct IconBuffer *dst, struct wlr_buffer *buf, int32_t scale) {
    if (buf == NULL || scale < 1 || buf->width < 1 || buf->width > ICON_MAX_SIZE
        || buf->width != buf->height) {
        return false;
    }

    void *data = NULL;
    uint32_t format = 0;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(
            buf, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride
        )) {
        return false;
    }

    const size_t row = (size_t)buf->width * ICON_BYTES_PER_PIXEL;
    uint8_t *pixels = NULL;

    if (data != NULL && format_supported(format) && stride >= row) {
        pixels = malloc(buffer_bytes(buf->width));
    }

    if (pixels != NULL) {
        // Row by row, stride may exceed width * 4.
        for (int32_t y = 0; y < buf->height; y++) {
            memcpy(
                pixels + (size_t)y * row,
                (const uint8_t *)data + (size_t)y * stride, row
            );
        }
    }

    wlr_buffer_end_data_ptr_access(buf);

    if (pixels == NULL) return false;

    *dst = (struct IconBuffer){
        .scale = scale,
        .size = buf->width,
        .format = format,
        .pixels = pixels,
    };

    return true;
}

static struct Window *window_for_toplevel(const struct wlr_xdg_toplevel *tl) {
    if (tl == NULL || tl->base == NULL) return NULL;

    const struct wlr_scene_tree *tree = tl->base->data;
    if (tree == NULL) return NULL;

    return window_from_scene_node((struct wlr_scene_node *)&tree->node);
}

static size_t count_buffers(const struct wl_list *list) {
    size_t n = 0;
    for (const struct wl_list *p = list->next;
         p != list && n < ICON_MAX_BUFFERS; p = p->next) {
        n++;
    }

    return n;
}

static void handle_set_icon(struct wl_listener *listener, void *data) {
    (void)listener;

    const struct wlr_xdg_toplevel_icon_manager_v1_set_icon_event *ev = data;
    struct Window *w = window_for_toplevel(ev->toplevel);
    if (w == NULL || w->closed) return;

    const struct wlr_xdg_toplevel_icon_v1 *src = ev->icon;

    // NULL/empty means "reset to default".
    if (src == NULL || (src->name == NULL && wl_list_empty(&src->buffers))) {
        window_icon_set_pending(w, NULL);
        return;
    }

    const size_t cap = count_buffers(&src->buffers);
    struct Icon *ic = icon_new(src->name, cap);
    if (ic == NULL) {
        log_warn("OOM while copying toplevel icon");
        return;
    }

    struct wlr_xdg_toplevel_icon_v1_buffer *b;
    wl_list_for_each(b, &src->buffers, link) {
        if (ic->buffer_count == cap) break;

        if (snapshot_buffer(
                &ic->buffers[ic->buffer_count], b->buffer, b->scale
            )) {
            ic->buffer_count++;
        } else {
            log_warn("Skipping unusable icon buffer");
        }
    }

    if (ic->name == NULL && ic->buffer_count == 0) {
        log_warn("Toplevel icon had no usable data (ignoring)");
        icon_destroy(ic);
        return;
    }

    window_icon_set_pending(w, ic); // takes ownership
}

bool icon_manager_init(struct wl_display *display) {
    if (manager != NULL) return true;

    manager = wlr_xdg_toplevel_icon_manager_v1_create(display, 1);
    if (manager == NULL) {
        log_warn("Failed to create xdg_toplevel_icon manager");
        return false;
    }

    set_icon_listener.notify = handle_set_icon;
    wl_signal_add(&manager->events.set_icon, &set_icon_listener);
    return true;
}

void icon_manager_finish(void) {
    if (manager == NULL) return;

    wl_list_remove(&set_icon_listener.link);
    manager = NULL;
}
