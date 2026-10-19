#pragma once

#include "server.h"
#include "wayland-server-core.h"

#include <stdbool.h>
#include <stdint.h>
#include <wayland-util.h>
#include <wlr/types/wlr_layer_shell_v1.h>

struct Output;
struct wlr_scene_tree;
struct wlr_surface;

struct LayerSurface {
    struct wlr_layer_surface_v1 *layer_surface;
    struct wlr_scene_tree *scene_tree;
    struct wlr_scene_tree *popup_tree;
    struct Output *output;
    struct NodeTag node_tag;

    struct wl_list link; // WindowManager.layer_surfaces

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener new_popup;

    uint32_t last_w, last_h;
    bool configured;
    bool mapped;
};

void layers_handle_new_surface(struct wl_listener *listener, void *data);
struct LayerSurface *layer_surface_from_wlr_surface(struct wlr_surface *surface
);
