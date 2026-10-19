#pragma once

#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wayland-server-core.h>

struct Popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wlr_scene_tree *tree;
    struct wlr_scene_tree *parent_tree;
    struct wlr_scene_shadow *shadow;
    struct wl_listener commit, destroy;
};

void popup_track(struct wlr_xdg_popup *, struct wlr_scene_tree *);
