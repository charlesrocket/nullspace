#include "output.h"

#include "layer.h"
#include "layout/param.h"
#include "log.h"
#include "manager.h"
#include "overview.h"
#include "seat.h"
#include "security.h"
#include "server.h"
#include "session.h"
#include "space.h"
#include "wallpaper.h"
#include "window.h"
#ifdef XWAYLAND
#include "xwayland.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-protocols/tearing-control-v1-enum.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

#define LOG_TOPIC "output"

static void output_handle_frame(struct wl_listener* l, void* data);
static void output_handle_request_state(struct wl_listener* l, void* data);
static void output_handle_destroy(struct wl_listener* l, void* data);

struct Output* output_focused(void) {
    struct Window* w = window_focused();
    if (w != NULL && w->output != NULL && !w->output->removed) {
        return w->output;
    }

    if (server.cursor != NULL && server.output_layout != NULL) {
        struct wlr_output* wo = wlr_output_layout_output_at(
            server.output_layout, server.cursor->x, server.cursor->y
        );

        if (wo != NULL) {
            struct Output* o = output_from_wlr_output(wo);
            if (o != NULL && !o->removed) return o;
        }
    }

    return output_primary();
}

bool output_set_enabled(struct wlr_output* wlr_output, bool on) {
    if (wlr_output->enabled == on) return true;

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, on);
    bool ok = wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    if (ok && on) wlr_output_schedule_frame(wlr_output);
    return ok;
}

// mode: 0 off | 1 on | 2 toggle
// name: NULL means all outputs
// (returns the number of outputs matched)
int output_set_power(const char* name, int mode) {
    int matched = 0;
    struct Output* o;
    wl_list_for_each(o, &server.outputs, link) {
        if (o->removed) continue;
        if (name != NULL && strcmp(o->wlr_output->name, name) != 0) continue;

        bool on = (mode == 2) ? !o->wlr_output->enabled : (mode == 1);
        if (output_set_enabled(o->wlr_output, on)) matched++;
    }

    if (matched > 0) {
        wm_mark_layout_dirty();
        output_update_manager_config();
    }

    return matched;
}

static void output_update_geometry(struct Output* out) {
    struct wlr_box box;
    wlr_output_layout_get_box(server.output_layout, out->wlr_output, &box);
    if (box.width <= 0 || box.height <= 0) return;

    out->pos_x = box.x;
    out->pos_y = box.y;
    out->width = box.width;
    out->height = box.height;

    out->area_x = box.x;
    out->area_y = box.y;
    out->area_width = box.width;
    out->area_height = box.height;
    out->area_set = true;
}

static void output_reassign_windows(struct Output* dying) {
    struct Output* new_home = NULL;
    struct Output* it;
    wl_list_for_each(it, &server.outputs, link) {
        if (it != dying && !it->removed) {
            new_home = it;
            break;
        }
    }

    struct Window* w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->closed || w->output != dying) continue;

        if (w->wlr_handle != NULL && dying->wlr_output != NULL) {
            wlr_foreign_toplevel_handle_v1_output_leave(
                w->wlr_handle, dying->wlr_output
            );
        }

        w->output = new_home;
        w->pos_valid = false;
        w->has_target = false;

        if (new_home != NULL && new_home->wlr_output != NULL
            && w->wlr_handle != NULL) {
            wlr_foreign_toplevel_handle_v1_output_enter(
                w->wlr_handle, new_home->wlr_output
            );
        }

        bool want_visible = (new_home != NULL) && w->mapped
                         && (w->space == new_home->current_space);

        if (want_visible && w->space_hidden) {
            w->space_hidden = false;
            w->anim.running = false;
            w->hide_dir = 0;
            w->reveal_dir = 0;
            if (w->saved_x != 0 || w->saved_y != 0) {
                window_set_position(w, w->saved_x, w->saved_y);
            }
        } else if (!want_visible && !w->space_hidden) {
            w->anim.running = false;
            w->hide_dir = 0;
            w->reveal_dir = 0;
            w->space_hidden = true;
            window_set_position(w, HIDDEN_POS_X, w->y);
        }
    }

    // Keyboard focus may have been on a now-hidden surface.
    seat_focus(NULL);
}

void output_update_manager_config(void) {
    if (server.output_manager == NULL) { return; }

    struct wlr_output_configuration_v1* out_config =
        wlr_output_configuration_v1_create();
    if (out_config == NULL) { return; }

    struct Output* out;
    wl_list_for_each(out, &server.outputs, link) {
        if (out->removed || out->wlr_output == NULL) { continue; }

        struct wlr_output_configuration_head_v1* head =
            wlr_output_configuration_head_v1_create(
                out_config, out->wlr_output
            );

        struct wlr_box box;
        wlr_output_layout_get_box(server.output_layout, out->wlr_output, &box);

        head->state.enabled = !wlr_box_empty(&box);
        head->state.x = box.x;
        head->state.y = box.y;
    }

    wlr_output_manager_v1_set_configuration(server.output_manager, out_config);
}

void output_create(struct wlr_output* wlr_output) {
    if (!wlr_output_init_render(
            wlr_output, server.allocator, server.renderer
        )) {
        log_warn("Failed to init render");

        return;
    }

    {
        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, true);

        struct wlr_output_mode* mode = wlr_output_preferred_mode(wlr_output);
        if (mode != NULL) {
            wlr_output_state_set_mode(&state, mode);
        } else if (wlr_output->width > 0 && wlr_output->height > 0) {
            wlr_output_state_set_custom_mode(
                &state, wlr_output->width, wlr_output->height,
                wlr_output->refresh
            );
        }

        bool ok = wlr_output_commit_state(wlr_output, &state);
        wlr_output_state_finish(&state);

        if (!ok) {
            log_err("Failed to commit initial state for %s", wlr_output->name);
            return;
        }
    }

    struct Output* out = calloc(1, sizeof(*out));
    if (out == NULL) return;

    out->wlr_output = wlr_output;
    out->current_space = 1; // DEFAULT
    out->scene_output = wlr_scene_output_create(server.scene, wlr_output);
    if (out->scene_output == NULL) {
        log_warn("Failed to create scene output");
        free(out);
        return;
    }

    struct wlr_output_layout_output* layout_out =
        wlr_output_layout_add_auto(server.output_layout, wlr_output);

    wlr_scene_output_layout_add_output(
        server.scene_layout, layout_out, out->scene_output
    );

    if (wlr_output->global != NULL) {
        security_sandbox_add(wlr_output->global);
    } else {
        log_warn(
            "wl_output global missing for '%s'; sandboxes will not see it",
            wlr_output->name
        );
    }
    output_update_geometry(out);
    output_update_manager_config();
    session_lock_update_geometry();

    out->frame.notify = output_handle_frame;
    wl_signal_add(&wlr_output->events.frame, &out->frame);

    out->request_state.notify = output_handle_request_state;
    wl_signal_add(&wlr_output->events.request_state, &out->request_state);

    out->destroy.notify = output_handle_destroy;
    wl_signal_add(&wlr_output->events.destroy, &out->destroy);

    out->wallpaper = wallpaper_output_create(out);
    wallpaper_update_output(out);

    wl_list_insert(&server.outputs, &out->link);

    { // adopt windows created before any output existed
        struct Window* w;
        wl_list_for_each(w, &wm.windows, link) {
            if (w->closed || w->output != NULL) continue;
            w->output = out;
            if (w->space == WINDOW_SPACE_UNSET) w->space = out->current_space;
            w->space_hidden = (w->space != out->current_space);

            if (w->wlr_handle != NULL) {
                wlr_foreign_toplevel_handle_v1_output_enter(
                    w->wlr_handle, out->wlr_output
                );
            }
        }
    }

    ext_workspace_output_enter(out);

    log_info(
        "Added '%s' (%dx%d @ %d,%d)", wlr_output->name, out->width, out->height,
        out->pos_x, out->pos_y
    );

    wm_mark_layout_dirty();

#ifdef XWAYLAND
    xwayland_primary_update();
#endif
}

static bool output_wants_tearing(struct Output* out) {
    if (server.tearing_control == NULL || server.overview_active
        || server.locked)
        return false;

    struct Window* w = window_focused();
    if (w == NULL || w->closed || !w->mapped || !w->fullscreen
        || w->space_hidden || w->output != out) {
        return false;
    }

    struct wlr_surface* s = window_wlr_surface(w);
    if (s == NULL) return false;

    return wlr_tearing_control_manager_v1_surface_hint_from_surface(
               server.tearing_control, s
           )
        == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
}

static void output_handle_frame(struct wl_listener* l, void* data) {
    (void)data;

    struct Output* out = wl_container_of(l, out, frame);

    if (wm.layout_dirty) {
        wm.layout_dirty = false;
        layout_apply();
        wallpaper_update_all();
    }

    if (server.overview_active) {
        overview_tick();
    } else {
        wm_animation_tick_all();
    }

    if (!wlr_scene_output_needs_frame(out->scene_output)) return;

    struct wlr_output_state state;
    wlr_output_state_init(&state);

    struct wlr_scene_output_state_options opts = {0};
    if (!wlr_scene_output_build_state(out->scene_output, &state, &opts)) {
        wlr_output_state_finish(&state);
        return;
    }

    if (output_wants_tearing(out)) {
        state.tearing_page_flip = true;
        if (!wlr_output_test_state(out->wlr_output, &state)) {
            state.tearing_page_flip = false;
        }
    }

    const bool committed = wlr_output_commit_state(out->wlr_output, &state);
    wlr_output_state_finish(&state);

    if (committed) session_lock_output_committed(out);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(out->scene_output, &now);

    if (committed && (wm_animation_active() || server.overview_active)) {
        wlr_output_schedule_frame(out->wlr_output);
    }
}

static void output_handle_request_state(struct wl_listener* l, void* data) {
    (void)l;

    struct wlr_output_event_request_state* ev = data;
    if (!wlr_output_commit_state(ev->output, ev->state)) return;

    struct Output* out = output_from_wlr_output(ev->output);
    if (out != NULL) {
        output_update_geometry(out);
        output_update_manager_config();
    }

    wm_mark_layout_dirty();
    session_lock_update_geometry();
}

static void output_handle_destroy(struct wl_listener* l, void* data) {
    (void)data;

    struct Output* out = wl_container_of(l, out, destroy);
    output_destroy(out);
}

void output_destroy(struct Output* out) {
    if (out->removed) return;
    out->removed = true;
    output_reassign_windows(out);

    struct LayerSurface *ls, *tmp_ls;
    wl_list_for_each_safe(ls, tmp_ls, &wm.layer_surfaces, link) {
        if (ls->output == out) {
            ls->output = NULL;
            wlr_layer_surface_v1_destroy(ls->layer_surface);
        }
    }

    if (out->wlr_output != NULL) {
        security_sandbox_remove(out->wlr_output->global);
    }

    if (out->wallpaper != NULL) {
        wallpaper_output_destroy(out->wallpaper);
        out->wallpaper = NULL;
    }

    wl_list_remove(&out->frame.link);
    wl_list_remove(&out->request_state.link);
    wl_list_remove(&out->destroy.link);
    wl_list_remove(&out->link);

    ext_workspace_output_leave(out);

    if (out->scene_output != NULL) {
        wlr_scene_output_destroy(out->scene_output);
    }

    if (out->wlr_output != NULL) {
        wlr_output_layout_remove(server.output_layout, out->wlr_output);
    }

    output_update_manager_config();
    session_lock_update_geometry();
    session_lock_outputs_changed();

    wm_mark_layout_dirty();
    log_info("Removed '%s'", out->wlr_output ? out->wlr_output->name : "?");

    free(out);

#ifdef XWAYLAND
    xwayland_primary_update();
#endif
}

struct Output* output_from_wlr_output(struct wlr_output* wlr_output) {
    struct Output* out;
    wl_list_for_each(out, &server.outputs, link) {
        if (out->wlr_output == wlr_output) return out;
    }

    return NULL;
}

struct Output* output_primary(void) {
    struct Output* out;
    wl_list_for_each(out, &server.outputs, link) {
        if (out->removed) continue;
        return out;
    }

    return NULL;
}
