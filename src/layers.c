#include "layers.h"

#include "manager.h"
#include "output.h"
#include "seat.h"
#include "server.h"

#include <scenefx/types/wlr_scene.h>
#include <stdint.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr-layer-shell-unstable-v1-protocol.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>

#define LOG_TOPIC "layer"

static struct wlr_scene_tree *
layer_tree_for(enum zwlr_layer_shell_v1_layer layer) {
    switch (layer) {
        case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
            return server.background_tree;
        case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: return server.bottom_tree;
        case ZWLR_LAYER_SHELL_V1_LAYER_TOP: return server.top_tree;
        case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY: return server.overlay_tree;
    }

    return server.top_tree;
}

static struct Output *output_for(struct wlr_layer_surface_v1 *layer) {
    if (layer->output != NULL) { return output_from_wlr_output(layer->output); }
    return output_primary();
}

static void
consume_exclusive(struct wlr_layer_surface_v1 *layer, struct wlr_box *box) {
    const struct wlr_layer_surface_v1_state *state = &layer->current;
    if (state->exclusive_zone <= 0) return;

    uint32_t edge = wlr_layer_surface_v1_get_exclusive_edge(layer);
    int32_t zone = state->exclusive_zone;

    switch (edge) {
        case WLR_EDGE_TOP:
            box->y += zone + state->margin.top;
            box->height -= zone + state->margin.top;
            break;
        case WLR_EDGE_BOTTOM: box->height -= zone + state->margin.bottom; break;
        case WLR_EDGE_LEFT:
            box->x += zone + state->margin.left;
            box->width -= zone + state->margin.left;
            break;
        case WLR_EDGE_RIGHT: box->width -= zone + state->margin.right; break;
        default: break;
    }

    if (box->width < 0) box->width = 0;
    if (box->height < 0) box->height = 0;
}

static struct wlr_box layer_bounds_for(struct LayerSurface *ls) {
    struct Output *o = ls->output;
    const struct wlr_layer_surface_v1_state *state =
        &ls->layer_surface->current;

    struct wlr_box full = {
        .x = o->pos_x,
        .y = o->pos_y,
        .width = o->width,
        .height = o->height,
    };

    if (state->exclusive_zone < 0) return full;

    if (state->exclusive_zone == 0) {
        struct wlr_box box = full;
        struct LayerSurface *it;

        wl_list_for_each(it, &wm.layer_surfaces, link) {
            if (it->output != o || !it->mapped) continue;
            consume_exclusive(it->layer_surface, &box);
        }

        return box;
    }

    struct wlr_box box = full;
    const uint32_t target = ls->layer_surface->current.layer;
    bool reached = false;

    for (int i = 3; i >= 0 && !reached; i--) {
        struct LayerSurface *it;
        wl_list_for_each(it, &wm.layer_surfaces, link) {
            if (it == ls) {
                reached = true;
                break;
            }

            if (it->output != o || !it->mapped) continue;
            if (it->layer_surface->current.layer != (uint32_t)i) continue;
            consume_exclusive(it->layer_surface, &box);
        }
    }

    (void)target;
    return box;
}

static void update_output_usable_area(struct Output *o) {
    if (o == NULL || o->removed) return;

    struct wlr_box box = {
        .x = o->pos_x,
        .y = o->pos_y,
        .width = o->width,
        .height = o->height,
    };

    struct LayerSurface *ls;
    wl_list_for_each(ls, &wm.layer_surfaces, link) {
        if (ls->output != o || !ls->mapped) continue;
        consume_exclusive(ls->layer_surface, &box);
    }

    int32_t w = box.width > 0 ? box.width : 1;
    int32_t h = box.height > 0 ? box.height : 1;

    if (o->area_set && o->area_x == box.x && o->area_y == box.y
        && o->area_width == w && o->area_height == h) {
        return;
    }

    o->area_x = box.x;
    o->area_y = box.y;
    o->area_width = w;
    o->area_height = h;
    o->area_set = true;

    wm_mark_layout_dirty();
}

static void arrange_layer_surface(struct LayerSurface *ls) {
    struct wlr_layer_surface_v1 *layer = ls->layer_surface;
    struct Output *o = ls->output;

    if (o == NULL || o->width <= 0 || o->height <= 0) return;

    const struct wlr_layer_surface_v1_state *state = &layer->current;
    struct wlr_box bounds = layer_bounds_for(ls);

    struct wlr_box box = {
        .x = 0,
        .y = 0,
        .width = (int32_t)state->desired_width,
        .height = (int32_t)state->desired_height,
    };

    const uint32_t both_h =
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    const uint32_t both_v =
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;

    if (box.width == 0) {
        box.x = bounds.x;
    } else if ((state->anchor & both_h) == both_h) {
        box.x = bounds.x + (bounds.width - box.width) / 2;
    } else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) {
        box.x = bounds.x;
    } else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
        box.x = bounds.x + bounds.width - box.width;
    } else {
        box.x = bounds.x + (bounds.width - box.width) / 2;
    }

    if (box.height == 0) {
        box.y = bounds.y;
    } else if ((state->anchor & both_v) == both_v) {
        box.y = bounds.y + (bounds.height - box.height) / 2;
    } else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) {
        box.y = bounds.y;
    } else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
        box.y = bounds.y + bounds.height - box.height;
    } else {
        box.y = bounds.y + (bounds.height - box.height) / 2;
    }

    if (box.width == 0) {
        box.x += state->margin.left;
        box.width = bounds.width - (state->margin.left + state->margin.right);
    } else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) {
        box.x += state->margin.left;
    } else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
        box.x -= state->margin.right;
    }

    if (box.height == 0) {
        box.y += state->margin.top;
        box.height = bounds.height - (state->margin.top + state->margin.bottom);
    } else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) {
        box.y += state->margin.top;
    } else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
        box.y -= state->margin.bottom;
    }

    if (box.width < 1) box.width = 1;
    if (box.height < 1) box.height = 1;

    wlr_scene_node_set_position(&ls->scene_tree->node, box.x, box.y);

    uint32_t cw = (uint32_t)box.width;
    uint32_t ch = (uint32_t)box.height;
    if (ls->configured && ls->last_w == cw && ls->last_h == ch) return;

    ls->configured = true;
    ls->last_w = cw;
    ls->last_h = ch;
    wlr_layer_surface_v1_configure(layer, cw, ch);
}

static void handle_map(struct wl_listener *lst, void *data) {
    (void)data;

    struct LayerSurface *ls = wl_container_of(lst, ls, map);

    ls->mapped = true;
    wlr_scene_node_set_enabled(&ls->scene_tree->node, true);

    arrange_layer_surface(ls);
    update_output_usable_area(ls->output);

    if (!server.locked
        && ls->layer_surface->current.keyboard_interactive
               == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
        wlr_seat_keyboard_notify_enter(
            server.seat, ls->layer_surface->surface, NULL, 0, NULL
        );
    }

    idle_update_inhibit();
}

static void handle_unmap(struct wl_listener *lst, void *data) {
    (void)data;

    struct LayerSurface *ls = wl_container_of(lst, ls, unmap);

    ls->mapped = false;
    wlr_scene_node_set_enabled(&ls->scene_tree->node, false);
    update_output_usable_area(ls->output);
    idle_update_inhibit();
    seat_focus(NULL);
}

static void handle_commit(struct wl_listener *lst, void *data) {
    (void)data;

    struct LayerSurface *ls = wl_container_of(lst, ls, commit);
    struct wlr_layer_surface_v1 *layer = ls->layer_surface;
    struct wlr_scene_tree *want = layer_tree_for(layer->current.layer);

    if (ls->scene_tree->node.parent != want) {
        wlr_scene_node_reparent(&ls->scene_tree->node, want);
    }

    arrange_layer_surface(ls);
}

static void handle_destroy(struct wl_listener *lst, void *data) {
    (void)data;

    struct LayerSurface *ls = wl_container_of(lst, ls, destroy);

    wl_list_remove(&ls->destroy.link);
    wl_list_remove(&ls->new_popup.link);
    wl_list_remove(&ls->map.link);
    wl_list_remove(&ls->unmap.link);
    wl_list_remove(&ls->commit.link);

    if (ls->scene_tree != NULL) {
        wlr_scene_node_destroy(&ls->scene_tree->node);
    }

    struct Output *o = ls->output;
    wl_list_remove(&ls->link);
    free(ls);

    if (o != NULL) update_output_usable_area(o);
}

static void handle_new_popup(struct wl_listener *lst, void *data) {
    struct wlr_xdg_popup *popup = data;
    struct LayerSurface *ls = wl_container_of(lst, ls, new_popup);

    popup_track(popup, ls->scene_tree);
}

struct LayerSurface *layer_surface_from_wlr_surface(struct wlr_surface *surface
) {
    if (surface == NULL) return NULL;

    struct LayerSurface *ls;
    wl_list_for_each(ls, &wm.layer_surfaces, link) {
        if (ls->layer_surface->surface == surface) return ls;
    }

    return NULL;
}

void layers_handle_new_surface(struct wl_listener *listener, void *data) {
    (void)listener;

    struct wlr_layer_surface_v1 *layer = data;

    struct LayerSurface *ls = calloc(1, sizeof(*ls));
    if (ls == NULL) return;

    ls->layer_surface = layer;
    ls->output = output_for(layer);
    ls->node_tag.type = NODE_LAYER_SURFACE;

    struct wlr_scene_tree *parent = layer_tree_for(layer->pending.layer);

    ls->scene_tree = wlr_scene_subsurface_tree_create(parent, layer->surface);
    if (ls->scene_tree == NULL) {
        free(ls);
        return;
    }

    ls->scene_tree->node.data = &ls->node_tag;
    wlr_scene_node_set_enabled(&ls->scene_tree->node, false);

    ls->map.notify = handle_map;
    ls->unmap.notify = handle_unmap;
    ls->commit.notify = handle_commit;
    ls->destroy.notify = handle_destroy;
    ls->new_popup.notify = handle_new_popup;

    wl_signal_add(&layer->surface->events.map, &ls->map);
    wl_signal_add(&layer->surface->events.unmap, &ls->unmap);
    wl_signal_add(&layer->surface->events.commit, &ls->commit);

    wl_signal_add(&layer->events.destroy, &ls->destroy);
    wl_signal_add(&layer->events.new_popup, &ls->new_popup);

    wl_list_insert(&wm.layer_surfaces, &ls->link);
}
