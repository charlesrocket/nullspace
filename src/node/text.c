#include "text.h"

#include <cairo/cairo.h>
#include <drm_fourcc.h>
#include <glib-object.h>
#include <glibconfig.h>
#include <limits.h>
#include <pango/pango-font.h>
#include <pango/pango-layout.h>
#include <pango/pango-types.h>
#include <pango/pangocairo.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

struct cairo_buffer {
    struct wlr_buffer base;
    cairo_surface_t *surface;
    void *data;
    size_t stride;
};

static void cairo_buffer_destroy(struct wlr_buffer *wlr_buffer) {
    struct cairo_buffer *buf = wl_container_of(wlr_buffer, buf, base);
    cairo_surface_destroy(buf->surface);
    free(buf->data);
    free(buf);
}

static bool cairo_buffer_begin_data_ptr_access(
    struct wlr_buffer *wlr_buffer, uint32_t flags, void **data,
    uint32_t *format, size_t *stride
) {
    (void)flags;
    struct cairo_buffer *buf = wl_container_of(wlr_buffer, buf, base);
    *data = buf->data;
    *format = DRM_FORMAT_ARGB8888;
    *stride = buf->stride;
    return true;
}

static void cairo_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
    (void)wlr_buffer;
}

static const struct wlr_buffer_impl cairo_buffer_impl = {
    .destroy = cairo_buffer_destroy,
    .begin_data_ptr_access = cairo_buffer_begin_data_ptr_access,
    .end_data_ptr_access = cairo_buffer_end_data_ptr_access,
};

static struct cairo_buffer *cairo_buffer_create(int width, int height) {
    if (width <= 0 || height <= 0) return NULL;

    struct cairo_buffer *buf = calloc(1, sizeof(*buf));
    if (!buf) return NULL;

    buf->stride = (size_t)width * 4;
    if ((size_t)height > SIZE_MAX / buf->stride) {
        free(buf);
        return NULL;
    }

    buf->data = calloc(1, buf->stride * (size_t)height);
    if (!buf->data) {
        free(buf);
        return NULL;
    }

    buf->surface = cairo_image_surface_create_for_data(
        buf->data, CAIRO_FORMAT_ARGB32, width, height, (int)buf->stride
    );

    if (cairo_surface_status(buf->surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(buf->surface);
        free(buf->data);
        free(buf);
        return NULL;
    }

    wlr_buffer_init(&buf->base, &cairo_buffer_impl, width, height);
    return buf;
}

static void handle_buffer_node_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct text_node *node = wl_container_of(l, node, node_destroy);
    wl_list_remove(&node->node_destroy.link);
    node->buffer_node = NULL;
}

static cairo_font_options_t *make_font_options(void) {
    cairo_font_options_t *o = cairo_font_options_create();

    cairo_font_options_set_antialias(o, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_subpixel_order(o, CAIRO_SUBPIXEL_ORDER_DEFAULT);
    cairo_font_options_set_hint_style(o, CAIRO_HINT_STYLE_FULL);
    cairo_font_options_set_hint_metrics(o, CAIRO_HINT_METRICS_ON);

    return o;
}

struct text_node *text_node_create(
    struct wlr_scene_tree *parent, const char *font_desc, const float color[4],
    int scale
) {
    if (scale < 1) scale = 1;

    struct text_node *node = calloc(1, sizeof(*node));
    if (!node) return NULL;

    node->buffer_node = wlr_scene_buffer_create(parent, NULL);
    if (!node->buffer_node) {
        free(node);
        return NULL;
    }

    node->font = pango_font_description_from_string(font_desc);
    if (!node->font) {
        wlr_scene_node_destroy(&node->buffer_node->node);
        free(node);
        return NULL;
    }

    node->text = strdup("");
    if (!node->text) {
        pango_font_description_free(node->font);
        wlr_scene_node_destroy(&node->buffer_node->node);
        free(node);
        return NULL;
    }

    memcpy(node->color, color, sizeof(node->color));
    node->scale = scale;

    node->node_destroy.notify = handle_buffer_node_destroy;
    wl_signal_add(&node->buffer_node->node.events.destroy, &node->node_destroy);

    return node;
}

static void measure_natural(struct text_node *node) {
    node->natural_w = 0;
    node->natural_h = 0;

    if (!node->text || node->text[0] == '\0' || !node->font) return;

    cairo_surface_t *scratch =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);

    cairo_t *cr = cairo_create(scratch);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_GRAY);

    PangoLayout *layout = pango_cairo_create_layout(cr);

    cairo_font_options_t *opts = make_font_options();
    pango_cairo_context_set_font_options(
        pango_layout_get_context(layout), opts
    );

    cairo_font_options_destroy(opts);

    pango_layout_set_font_description(layout, node->font);
    pango_layout_set_text(layout, node->text, -1);
    pango_layout_set_width(layout, -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);

    int w = 0, h = 0;
    pango_layout_get_pixel_size(layout, &w, &h);
    node->natural_w = w;
    node->natural_h = h;

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(scratch);
}

static void render(struct text_node *node) {
    if (!node || !node->buffer_node) return;

    if (!node->text || node->text[0] == '\0' || !node->font) {
        wlr_scene_buffer_set_buffer(node->buffer_node, NULL);
        wlr_buffer_drop(node->buffer);
        node->buffer = NULL;
        node->surface = NULL;
        node->rendered_w = 0;
        node->rendered_h = 0;
        return;
    }

    cairo_surface_t *scratch =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);

    cairo_t *scratch_cr = cairo_create(scratch);
    cairo_set_antialias(scratch_cr, CAIRO_ANTIALIAS_GRAY);

    PangoLayout *layout = pango_cairo_create_layout(scratch_cr);

    cairo_font_options_t *opts = make_font_options();
    pango_cairo_context_set_font_options(
        pango_layout_get_context(layout), opts
    );

    PangoFontDescription *font = pango_font_description_copy(node->font);
    int sz = pango_font_description_get_size(font);
    if (pango_font_description_get_size_is_absolute(font)) {
        pango_font_description_set_absolute_size(font, sz * node->scale);
    } else {
        pango_font_description_set_size(font, sz * node->scale);
    }

    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    pango_layout_set_text(layout, node->text, -1);

    if (node->max_width > 0) {
        gint64 wrap_px =
            (gint64)node->max_width * (gint64)node->scale * (gint64)PANGO_SCALE;
        if (wrap_px > (gint64)G_MAXINT) wrap_px = (gint64)G_MAXINT;
        if (wrap_px < 0) wrap_px = 0;
        pango_layout_set_width(layout, (int)wrap_px);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    } else {
        pango_layout_set_width(layout, -1);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    }

    int scaled_w = 0, scaled_h = 0;
    pango_layout_get_pixel_size(layout, &scaled_w, &scaled_h);

    if (scaled_w <= 0 || scaled_h <= 0) {
        cairo_font_options_destroy(opts);
        g_object_unref(layout);
        cairo_destroy(scratch_cr);
        cairo_surface_destroy(scratch);
        return;
    }

    node->rendered_w = scaled_w / node->scale;
    node->rendered_h = scaled_h / node->scale;

    int surf_w = scaled_w;
    int surf_h = scaled_h;

    if (!node->buffer || node->buffer->width != surf_w
        || node->buffer->height != surf_h) {
        wlr_buffer_drop(node->buffer);
        node->buffer = NULL;
        node->surface = NULL;

        struct cairo_buffer *cbuf = cairo_buffer_create(surf_w, surf_h);
        if (!cbuf) {
            cairo_font_options_destroy(opts);
            g_object_unref(layout);
            cairo_destroy(scratch_cr);
            cairo_surface_destroy(scratch);
            return;
        }

        node->buffer = &cbuf->base;
        node->surface = cbuf->surface;
    }

    cairo_t *draw_cr = cairo_create(node->surface);
    cairo_set_font_options(draw_cr, opts);
    cairo_font_options_destroy(opts);
    cairo_set_antialias(draw_cr, CAIRO_ANTIALIAS_GRAY);

    cairo_set_operator(draw_cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(draw_cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(draw_cr);

    cairo_set_operator(draw_cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(
        draw_cr, (double)node->color[0], (double)node->color[1],
        (double)node->color[2], (double)node->color[3]
    );

    cairo_move_to(draw_cr, 0.0, 0.0);
    pango_cairo_show_layout(draw_cr, layout);
    cairo_destroy(draw_cr);

    cairo_surface_flush(node->surface);

    wlr_scene_buffer_set_buffer(node->buffer_node, node->buffer);
    wlr_scene_buffer_set_dest_size(
        node->buffer_node, node->rendered_w, node->rendered_h
    );

    g_object_unref(layout);
    cairo_destroy(scratch_cr);
    cairo_surface_destroy(scratch);
}

void text_node_set_text(struct text_node *node, const char *text) {
    if (!node) return;

    const char *want = text ? text : "";
    if (node->text && strcmp(node->text, want) == 0) return;

    char *copy = strdup(want);
    if (!copy) return;

    free(node->text);
    node->text = copy;

    measure_natural(node);
    if (node->buffer_node) render(node);
}

void text_node_set_color(struct text_node *node, const float color[4]) {
    if (!node) return;
    if (memcmp(node->color, color, sizeof(node->color)) == 0) return;

    memcpy(node->color, color, sizeof(node->color));
    if (node->buffer_node) render(node);
}

void text_node_set_max_width(struct text_node *node, int max_width) {
    if (!node) return;
    if (max_width < 0) max_width = 0;
    if (node->max_width == max_width) return;

    node->max_width = max_width;
    if (node->buffer_node) render(node);
}

int text_node_natural_width(const struct text_node *node) {
    return node ? node->natural_w : 0;
}

void text_node_get_size(struct text_node *node, int *w, int *h) {
    if (w) *w = node ? node->rendered_w : 0;
    if (h) *h = node ? node->rendered_h : 0;
}

void text_node_destroy(struct text_node *node) {
    if (!node) return;

    if (node->buffer_node) wlr_scene_node_destroy(&node->buffer_node->node);

    wlr_buffer_drop(node->buffer);
    pango_font_description_free(node->font);
    free(node->text);
    free(node);
}
