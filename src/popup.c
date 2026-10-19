#include "config.h"
#include "layers.h"
#include "output.h"
#include "server.h"
#include "session.h"
#include "popup.h"

#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>

static const float popup_shadow_color[4] = {
    0.0f, 0.0f, 0.0f, CFG_POPUP_SHADOW_OPACITY
};

static void popup_update_shadow(struct Popup *p) {
    if (p == NULL || p->shadow == NULL || p->xdg_popup == NULL) { return; }

    struct wlr_xdg_surface *xdg_surface = p->xdg_popup->base;
    int width = xdg_surface->geometry.width;
    int height = xdg_surface->geometry.height;

    if (width <= 0 || height <= 0) { return; }

    const int sigma = (int)p->shadow->blur_sigma;

    wlr_scene_shadow_set_size(p->shadow, width + 2 * sigma, height + 2 * sigma);

    wlr_scene_node_set_position(
        &p->shadow->node, CFG_POPUP_SHADOW_OFFSET_X - sigma,
        CFG_POPUP_SHADOW_OFFSET_Y - sigma
    );

    wlr_scene_node_lower_to_bottom(&p->shadow->node);
}

static void popup_handle_commit(struct wl_listener *l, void *data) {
    (void)data;

    struct Popup *p = wl_container_of(l, p, commit);

    popup_update_shadow(p);

    if (!p->xdg_popup->base->initial_commit) { return; }

    struct Output *o = output_primary();
    if (o != NULL && p->parent_tree != NULL) {
        int lx = 0, ly = 0;
        wlr_scene_node_coords(&p->parent_tree->node, &lx, &ly);

        struct wlr_box box = {
            .x = o->pos_x - lx,
            .y = o->pos_y - ly,
            .width = o->width,
            .height = o->height,
        };

        wlr_xdg_popup_unconstrain_from_box(p->xdg_popup, &box);
    }

    wlr_xdg_surface_schedule_configure(p->xdg_popup->base);
}

static void popup_handle_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct Popup *p = wl_container_of(l, p, destroy);

    wl_list_remove(&p->commit.link);
    wl_list_remove(&p->destroy.link);

    free(p);
}

void popup_track(
    struct wlr_xdg_popup *xdg_popup, struct wlr_scene_tree *parent_tree
) {
    struct Popup *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        wlr_xdg_popup_destroy(xdg_popup);
        return;
    }

    struct wlr_scene_tree *tree =
        wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
    if (tree == NULL) {
        free(p);
        wlr_xdg_popup_destroy(xdg_popup);
        return;
    }

    p->tree = tree;

    p->shadow = wlr_scene_shadow_create(
        tree, 0, 0, CFG_POPUP_SHADOW_RADIUS, CFG_POPUP_SHADOW_SIGMA,
        popup_shadow_color
    );

    if (p->shadow == NULL) {
        wlr_scene_node_destroy(&tree->node);
        free(p);

        wlr_xdg_popup_destroy(xdg_popup);
        return;
    }

    wlr_scene_node_lower_to_bottom(&p->shadow->node);

    xdg_popup->base->data = tree;

    p->xdg_popup = xdg_popup;
    p->parent_tree = parent_tree;

    p->commit.notify = popup_handle_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &p->commit);

    p->destroy.notify = popup_handle_destroy;
    wl_signal_add(&xdg_popup->base->events.destroy, &p->destroy);
}
