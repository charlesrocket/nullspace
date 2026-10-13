#include "output.h"

#include "layouts/layout.h"
#include "log.h"
#include "manager.h"
#include "server.h"
#ifdef WALLPAPER
#include "wallpaper.h"
#endif

#include <stdlib.h>
#include <sys/_clock_id.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

#define LOG_TOPIC "output"

static void output_handle_frame(struct wl_listener *l, void *data);
static void output_handle_request_state(struct wl_listener *l, void *data);
static void output_handle_destroy(struct wl_listener *l, void *data);

void output_create(struct wlr_output *wlr_output) {
    if (!wlr_output_init_render(
            wlr_output, server.allocator, server.renderer
        )) {
        wlr_log(WLR_ERROR, "Failed to init render");

        return;
    }

    struct Output *out = calloc(1, sizeof(*out));
    if (out == NULL) return;

    out->wlr_output = wlr_output;
    out->scene_output = wlr_scene_output_create(server.scene, wlr_output);
    if (out->scene_output == NULL) {
        log_warn("Failed to create scene output");
        free(out);
        return;
    }

    struct wlr_output_layout_output *layout_out =
        wlr_output_layout_add_auto(server.output_layout, wlr_output);

    wlr_scene_output_layout_add_output(
        server.scene_layout, layout_out, out->scene_output
    );

    struct wlr_box box;
    wlr_output_layout_get_box(server.output_layout, wlr_output, &box);

    out->pos_x = box.x;
    out->pos_y = box.y;
    out->width = box.width;
    out->height = box.height;
    out->area_x = out->pos_x;
    out->area_y = out->pos_y;
    out->area_width = out->width;
    out->area_height = out->height;
    out->area_set = true;

    out->frame.notify = output_handle_frame;
    wl_signal_add(&wlr_output->events.frame, &out->frame);

    out->request_state.notify = output_handle_request_state;
    wl_signal_add(&wlr_output->events.request_state, &out->request_state);

    out->destroy.notify = output_handle_destroy;
    wl_signal_add(&wlr_output->events.destroy, &out->destroy);

#ifdef WALLPAPER
    out->wallpaper = wallpaper_output_create(out);
    wallpaper_update_output(out);
#endif

    wl_list_insert(&server.outputs, &out->link);

    log_info(
        "Added %s (%dx%d @ %d,%d)", wlr_output->name, out->width, out->height,
        out->pos_x, out->pos_y
    );

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != NULL) {
        wlr_output_state_set_mode(&state, mode);
    } else { // TODO
        wlr_output_state_set_custom_mode(&state, 1920, 1080, 0);
    }

    if (!wlr_scene_output_build_state(out->scene_output, &state, NULL)) {
        log_err("Failed to build initial state for %s", wlr_output->name);
    } else if (!wlr_output_commit_state(wlr_output, &state)) {
        log_err("Failed to commit initial state for %s", wlr_output->name);
    }

    wm_mark_layout_dirty();
    wlr_output_state_finish(&state);
}

static void output_handle_frame(struct wl_listener *l, void *data) {
    (void)data;

    struct Output *out = wl_container_of(l, out, frame);

    if (wm.layout_dirty) {
        wm.layout_dirty = false;
        layout_apply();

#ifdef WALLPAPER
        // wallpaper_update_output(out);
        wallpaper_update_all();
#endif
    }

    wm_animation_tick_all();

#ifdef WALLPAPER
    if (wallpaper_blur_wanted()) {
        wlr_damage_ring_add_whole(&out->scene_output->damage_ring);
    }
#endif

    if (!wlr_scene_output_commit(out->scene_output, NULL)) {
        log_warn("Scene commit failed on %s", out->wlr_output->name);
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(out->scene_output, &now);

    if (wm_animation_active()) wlr_output_schedule_frame(out->wlr_output);
}

static void output_handle_request_state(struct wl_listener *l, void *data) {
    (void)l;

    struct wlr_output_event_request_state *ev = data;
    wlr_output_commit_state(ev->output, ev->state);
}

static void output_handle_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct Output *out = wl_container_of(l, out, destroy);
    output_destroy(out);
}

void output_destroy(struct Output *out) {
    if (out->removed) return;
    out->removed = true;

#ifdef WALLPAPER
    if (out->wallpaper != NULL) {
        wallpaper_output_destroy(out->wallpaper);
        out->wallpaper = NULL;
    }
#endif

    wl_list_remove(&out->frame.link);
    wl_list_remove(&out->request_state.link);
    wl_list_remove(&out->destroy.link);
    wl_list_remove(&out->link);

    if (out->scene_output != NULL) {
        wlr_scene_output_destroy(out->scene_output);
    }

    if (out->wlr_output != NULL) {
        wlr_output_layout_remove(server.output_layout, out->wlr_output);
    }

    wm_mark_layout_dirty();
    log_info("Removed %s", out->wlr_output ? out->wlr_output->name : "?");

    free(out);
}

struct Output *output_primary(void) {
    struct Output *out;
    wl_list_for_each(out, &server.outputs, link) {
        if (out->removed) continue;
        if (out->width <= 0 || out->height <= 0) continue;

        return out;
    }

    return NULL;
}
