#pragma once

#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>

struct Popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wlr_scene_tree *tree, *root_tree, *parent_tree;
    struct wlr_scene_shadow *shadow;
    struct wl_listener commit, destroy, reposition;
    struct wl_list link;
};

void popup_track(struct wlr_xdg_popup *, struct wlr_scene_tree *);
