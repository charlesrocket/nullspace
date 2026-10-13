#include "background.h"

#include "ext-background-effect-v1-protocol.h"
#include "log.h"
#include "server.h"

#include <pixman.h>
#include <scenefx/types/wlr_scene.h>
#include <stdint.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>

#define LOG_TOPIC          "background-effect"

#define BG_BLUR_PASSES     2
#define BG_BLUR_RADIUS     10
#define BG_BLUR_NOISE      0.02f
#define BG_BLUR_BRIGHTNESS 1.0f
#define BG_BLUR_CONTRAST   1.0f
#define BG_BLUR_SATURATION 1.0f
#define BG_BLUR_STRENGTH   1.0f
#define BG_BLUR_ALPHA      1.0f

struct BgEffectRect {
    struct wl_list link;
    struct wlr_scene_blur *blur;
    struct wl_listener destroy;
};

struct BgEffectSurface {
    struct wl_resource *resource; // ?NULL
    struct wlr_surface *surface;  // ?NULL

    struct wl_listener surface_destroy;
    struct wl_listener surface_commit;

    struct wlr_scene_tree *blur_tree;
    struct wl_listener blur_tree_destroy;

    struct wl_list rects;
    struct wl_list link; // bg_effect_surfaces

    pixman_region32_t pending_blur;
    pixman_region32_t current_blur;
    pixman_region32_t applied;
    int applied_x, applied_y;
};

static struct wl_list bg_effect_surfaces;
static struct wl_global *bg_effect_global;

static const struct ext_background_effect_surface_v1_interface surface_impl;
static const struct ext_background_effect_manager_v1_interface manager_impl;

static struct wlr_scene_node *
find_surface_node(struct wlr_scene_tree *root, struct wlr_surface *surface) {
    struct wlr_scene_node *child;
    wl_list_for_each(child, &root->children, link) {
        if (child->type == WLR_SCENE_NODE_TREE) {
            struct wlr_scene_node *found =
                find_surface_node(wlr_scene_tree_from_node(child), surface);

            if (found != NULL) return found;
        } else if (child->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(
                wlr_scene_buffer_from_node(child)
            );

            if (ss != NULL && ss->surface == surface) return child;
        }
    }

    return NULL;
}

static void handle_rect_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct BgEffectRect *r = wl_container_of(l, r, destroy);
    wl_list_remove(&r->destroy.link);
    wl_list_remove(&r->link);
    free(r);
}

static void clear_rects(struct BgEffectSurface *bg) {
    struct BgEffectRect *r, *tmp;
    wl_list_for_each_safe(r, tmp, &bg->rects, link) {
        wlr_scene_node_destroy(&r->blur->node);
    }

    pixman_region32_clear(&bg->applied);
}

static void handle_blur_tree_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct BgEffectSurface *bg = wl_container_of(l, bg, blur_tree_destroy);
    wl_list_remove(&bg->blur_tree_destroy.link);
    bg->blur_tree = NULL;
}

static struct wlr_scene_tree *ensure_blur_tree(struct BgEffectSurface *bg) {
    if (bg->blur_tree != NULL) return bg->blur_tree;
    if (bg->surface == NULL) return NULL;

    struct wlr_scene_node *surface_node =
        find_surface_node(&server.scene->tree, bg->surface);

    if (surface_node == NULL || surface_node->parent == NULL) return NULL;

    struct wlr_scene_tree *blur_tree =
        wlr_scene_tree_create(surface_node->parent);
    if (blur_tree == NULL) return NULL;

    wlr_scene_node_lower_to_bottom(&blur_tree->node);

    bg->blur_tree = blur_tree;
    bg->blur_tree_destroy.notify = handle_blur_tree_destroy;
    wl_signal_add(&blur_tree->node.events.destroy, &bg->blur_tree_destroy);

    return blur_tree;
}

static void blur_nodes_update(struct BgEffectSurface *bg) {
    if (bg->surface == NULL) return;

    pixman_region32_t region;
    pixman_region32_init(&region);

    int sw = bg->surface->current.width;
    int sh = bg->surface->current.height;

    if (sw > 0 && sh > 0 && pixman_region32_not_empty(&bg->current_blur)) {
        pixman_region32_t box;
        pixman_region32_init_rect(
            &box, 0, 0, (unsigned int)sw, (unsigned int)sh
        );

        pixman_region32_intersect(&region, &box, &bg->current_blur);
        pixman_region32_fini(&box);
    }

    if (!pixman_region32_not_empty(&region)) {
        clear_rects(bg);
        pixman_region32_fini(&region);
        return;
    }

    struct wlr_scene_tree *tree = ensure_blur_tree(bg);
    if (tree == NULL) {
        pixman_region32_fini(&region);
        return;
    }

    struct wlr_scene_node *surface_node =
        find_surface_node(&server.scene->tree, bg->surface);
    if (surface_node == NULL) {
        pixman_region32_fini(&region);
        return;
    }

    int offset_x = surface_node->x;
    int offset_y = surface_node->y;

    if (!wl_list_empty(&bg->rects) && bg->applied_x == offset_x
        && bg->applied_y == offset_y
        && pixman_region32_equal(&bg->applied, &region)) {
        pixman_region32_fini(&region);
        return;
    }

    clear_rects(bg);

    int nrects = 0;
    const pixman_box32_t *rects = pixman_region32_rectangles(&region, &nrects);

    for (int i = 0; i < nrects; i++) {
        int x = offset_x + rects[i].x1;
        int y = offset_y + rects[i].y1;
        int w = rects[i].x2 - rects[i].x1;
        int h = rects[i].y2 - rects[i].y1;
        if (w <= 0 || h <= 0) continue;

        struct wlr_scene_blur *blur = wlr_scene_blur_create(tree, w, h);
        if (blur == NULL) continue;

        wlr_scene_node_set_position(&blur->node, x, y);
        wlr_scene_blur_set_strength(blur, BG_BLUR_STRENGTH);
        wlr_scene_blur_set_alpha(blur, BG_BLUR_ALPHA);

        struct BgEffectRect *r = calloc(1, sizeof(*r));
        if (r == NULL) {
            wlr_scene_node_destroy(&blur->node);
            continue;
        }

        r->blur = blur;
        r->destroy.notify = handle_rect_destroy;
        wl_signal_add(&blur->node.events.destroy, &r->destroy);
        wl_list_insert(&bg->rects, &r->link);
    }

    pixman_region32_copy(&bg->applied, &region);

    bg->applied_x = offset_x;
    bg->applied_y = offset_y;

    pixman_region32_fini(&region);
}

static void detach_from_surface(struct BgEffectSurface *bg) {
    if (bg->surface == NULL) return;

    wl_list_remove(&bg->surface_destroy.link);
    wl_list_remove(&bg->surface_commit.link);

    if (bg->blur_tree != NULL) { wlr_scene_node_destroy(&bg->blur_tree->node); }

    clear_rects(bg);

    bg->surface = NULL;
}

static void handle_surface_commit(struct wl_listener *l, void *data) {
    (void)data;

    struct BgEffectSurface *bg = wl_container_of(l, bg, surface_commit);

    if (!pixman_region32_equal(&bg->current_blur, &bg->pending_blur)) {
        pixman_region32_copy(&bg->current_blur, &bg->pending_blur);
    }

    blur_nodes_update(bg);
}

static void handle_surface_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct BgEffectSurface *bg = wl_container_of(l, bg, surface_destroy);
    detach_from_surface(bg);
}

static void handle_resource_destroy(struct wl_resource *resource) {
    struct BgEffectSurface *bg = wl_resource_get_user_data(resource);
    if (bg == NULL) return;

    wl_resource_set_user_data(resource, NULL);
    bg->resource = NULL;

    detach_from_surface(bg);

    wl_list_remove(&bg->link);
    pixman_region32_fini(&bg->applied);
    pixman_region32_fini(&bg->pending_blur);
    pixman_region32_fini(&bg->current_blur);
    free(bg);
}

static void
surface_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;

    wl_resource_destroy(resource);
}

static void surface_handle_set_blur_region(
    struct wl_client *client, struct wl_resource *resource,
    struct wl_resource *region_resource
) {
    (void)client;

    struct BgEffectSurface *bg = wl_resource_get_user_data(resource);
    if (bg == NULL || bg->surface == NULL) {
        wl_resource_post_error(
            resource, EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED,
            "wl_surface object has been destroyed"
        );

        return;
    }

    pixman_region32_clear(&bg->pending_blur);

    if (region_resource != NULL) {
        const pixman_region32_t *region =
            wlr_region_from_resource(region_resource);

        if (region != NULL) { pixman_region32_copy(&bg->pending_blur, region); }
    }
}

static const struct ext_background_effect_surface_v1_interface surface_impl = {
    .destroy = surface_handle_destroy,
    .set_blur_region = surface_handle_set_blur_region,
};

static void
manager_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void manager_handle_get_background_effect(
    struct wl_client *client, struct wl_resource *resource, uint32_t id,
    struct wl_resource *surface_resource
) {
    struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

    struct BgEffectSurface *it;
    wl_list_for_each(it, &bg_effect_surfaces, link) {
        if (it->surface == surface) {
            wl_resource_post_error(
                resource,
                EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
                "wl_surface object already has an "
                "ext_background_effect_surface_v1 object"
            );

            return;
        }
    }

    struct BgEffectSurface *bg = calloc(1, sizeof(*bg));
    if (bg == NULL) {
        wl_client_post_no_memory(client);
        return;
    }

    struct wl_resource *res = wl_resource_create(
        client, &ext_background_effect_surface_v1_interface, 1, id
    );
    if (res == NULL) {
        free(bg);
        wl_client_post_no_memory(client);
        return;
    }

    bg->resource = res;
    bg->surface = surface;
    pixman_region32_init(&bg->pending_blur);
    pixman_region32_init(&bg->current_blur);
    pixman_region32_init(&bg->applied);
    wl_list_init(&bg->rects);

    bg->surface_commit.notify = handle_surface_commit;
    wl_signal_add(&surface->events.commit, &bg->surface_commit);

    bg->surface_destroy.notify = handle_surface_destroy;
    wl_signal_add(&surface->events.destroy, &bg->surface_destroy);

    wl_resource_set_implementation(
        res, &surface_impl, bg, handle_resource_destroy
    );

    wl_list_insert(&bg_effect_surfaces, &bg->link);
}

static const struct ext_background_effect_manager_v1_interface manager_impl = {
    .destroy = manager_handle_destroy,
    .get_background_effect = manager_handle_get_background_effect,
};

static void manager_bind(
    struct wl_client *client, void *data, uint32_t version, uint32_t id
) {
    (void)data;
    if (version > 1) version = 1;

    struct wl_resource *resource = wl_resource_create(
        client, &ext_background_effect_manager_v1_interface, (int)version, id
    );

    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
    ext_background_effect_manager_v1_send_capabilities(
        resource, EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR
    );
}

bool background_effect_init(struct wl_display *display) {
    wl_list_init(&bg_effect_surfaces);

    bg_effect_global = wl_global_create(
        display, &ext_background_effect_manager_v1_interface, 1, NULL,
        manager_bind
    );

    if (bg_effect_global == NULL) {
        log_warn("Failed to create background-effect");
        return false;
    }

    wlr_scene_set_blur_data(
        server.scene, BG_BLUR_PASSES, BG_BLUR_RADIUS, BG_BLUR_NOISE,
        BG_BLUR_BRIGHTNESS, BG_BLUR_CONTRAST, BG_BLUR_SATURATION
    );

    return true;
}

void background_effect_finish(void) {
    if (bg_effect_global != NULL) {
        wl_global_destroy(bg_effect_global);
        bg_effect_global = NULL;
    }
}
