#pragma once

#include <cairo/cairo.h>
#include <pango/pango-font.h>
#include <pango/pangocairo.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_scene.h>

struct text_node {
    struct wlr_scene_buffer *buffer_node;
    struct wlr_buffer *buffer;
    cairo_surface_t *surface;
    PangoFontDescription *font;
    char *text;
    float color[4];
    int scale;

    int natural_w;
    int natural_h;
    int max_width;
    int rendered_w;
    int rendered_h;

    struct wl_listener node_destroy;
};

struct text_node *text_node_create(
    struct wlr_scene_tree *parent, const char *font_desc, const float color[4],
    int scale
);

void text_node_set_text(struct text_node *node, const char *text);
void text_node_set_color(struct text_node *node, const float color[4]);
void text_node_set_max_width(struct text_node *node, int max_width);

int text_node_natural_width(const struct text_node *node);

void text_node_get_size(struct text_node *node, int *w, int *h);

void text_node_destroy(struct text_node *node);
