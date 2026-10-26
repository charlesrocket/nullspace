#include "popup.h"

#include "config.h"
#include "layers.h"
#include "output.h"
#include "server.h"
#include "session.h"

#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

static const float popup_shadow_color[4] = {
    0.0f, 0.0f, 0.0f, CFG_POPUP_SHADOW_OPACITY
};

static struct wl_list popups = {&popups, &popups};

static struct Popup *popup_find(struct wlr_xdg_surface *base) {
    struct Popup *it;
    wl_list_for_each(it, &popups, link) {
        if (it->xdg_popup->base == base) { return it; }
    }

    return NULL;
}

static struct wlr_scene_tree *popup_root_for(
    struct wlr_xdg_popup *xdg_popup, struct wlr_scene_tree *parent_tree
) {
    if (xdg_popup->parent != NULL) {
        struct wlr_xdg_surface *parent =
            wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);

        if (parent != NULL && parent->role == WLR_XDG_SURFACE_ROLE_POPUP) {
            struct Popup *pp = popup_find(parent);
            if (pp != NULL) { return pp->root_tree; }
        }
    }

    return parent_tree;
}

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

static void popup_unconstrain(struct Popup *p) {
    if (p->root_tree != NULL) {
        int lx = 0, ly = 0;
        wlr_scene_node_coords(&p->root_tree->node, &lx, &ly);

        struct Output *o = NULL;
        struct wlr_output *wo =
            wlr_output_layout_output_at(server.output_layout, lx, ly);

        if (wo != NULL) { o = output_from_wlr_output(wo); }
        if (o == NULL) { o = output_primary(); }

        if (o != NULL) {
            struct wlr_box box = {
                .x = o->pos_x - lx,
                .y = o->pos_y - ly,
                .width = o->width,
                .height = o->height,
            };

            wlr_xdg_popup_unconstrain_from_box(p->xdg_popup, &box);
        }
    }

    wlr_xdg_surface_schedule_configure(p->xdg_popup->base);
}

static void popup_handle_commit(struct wl_listener *l, void *data) {
    (void)data;

    struct Popup *p = wl_container_of(l, p, commit);

    popup_update_shadow(p);

    if (!p->xdg_popup->base->initial_commit) { return; }

    popup_unconstrain(p);
}

static void popup_handle_reposition(struct wl_listener *l, void *data) {
    (void)data;

    struct Popup *p = wl_container_of(l, p, reposition);

    if (!p->xdg_popup->base->initialized) { return; }

    popup_unconstrain(p);
}

static void popup_handle_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct Popup *p = wl_container_of(l, p, destroy);

    wl_list_remove(&p->commit.link);
    wl_list_remove(&p->reposition.link);
    wl_list_remove(&p->destroy.link);
    wl_list_remove(&p->link);

    free(p);
}

void popup_track(
    struct wlr_xdg_popup *xdg_popup, struct wlr_scene_tree *parent_tree
) {
    if (popup_find(xdg_popup->base) != NULL) { return; }

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
    p->root_tree = popup_root_for(xdg_popup, parent_tree);

    p->commit.notify = popup_handle_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &p->commit);

    p->reposition.notify = popup_handle_reposition;
    wl_signal_add(&xdg_popup->events.reposition, &p->reposition);

    p->destroy.notify = popup_handle_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &p->destroy);

    wl_list_insert(&popups, &p->link);
}
