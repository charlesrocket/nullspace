#include "activation.h"

#include "config.h"
#include "log.h"
#include "manager.h"
#include "seat.h"
#include "server.h"
#include "window.h"

#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

#define LOG_TOPIC "activation"

struct ActivationContext {
    struct wlr_xdg_activation_token_v1 *token;

    struct wlr_surface *origin_focus;
    struct wl_listener origin_focus_destroy;

    struct wlr_seat_client *seat_client; // ?NULL

    struct wl_listener token_destroy;

    int space;
    bool is_compositor;
};

static struct wl_listener new_token_listener;
static struct wl_listener request_activate_listener;
static bool activation_initialized;

static void context_free(struct ActivationContext *ctx) {
    if (ctx == NULL) return;

    if (ctx->origin_focus != NULL) {
        wl_list_remove(&ctx->origin_focus_destroy.link);
    }

    wl_list_remove(&ctx->token_destroy.link);
    free(ctx);
}

static void handle_token_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct ActivationContext *ctx = wl_container_of(l, ctx, token_destroy);
    ctx->token = NULL;
    context_free(ctx);
}

static void handle_origin_focus_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct ActivationContext *ctx =
        wl_container_of(l, ctx, origin_focus_destroy);

    ctx->origin_focus = NULL;
    wl_list_remove(&ctx->origin_focus_destroy.link);
    wl_list_init(&ctx->origin_focus_destroy.link);
}

static struct Window *window_from_surface(struct wlr_surface *surface) {
    if (surface == NULL) return NULL;

    struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(surface);
    if (xdg == NULL || xdg->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) return NULL;

    struct wlr_scene_tree *tree = xdg->data;
    if (tree == NULL) return NULL;

    return window_from_scene_node(&tree->node);
}

static struct wlr_seat_client *
seat_client_for_token(struct wlr_xdg_activation_token_v1 *token) {
    if (token == NULL || token->surface == NULL
        || token->surface->resource == NULL) {
        return NULL;
    }

    struct wl_client *client = wl_resource_get_client(token->surface->resource);

    struct wlr_seat_client *sc;
    wl_list_for_each(sc, &server.seat->clients, link) {
        if (sc->client == client) return sc;
    }

    return NULL;
}

static void handle_request_activate(struct wl_listener *l, void *data) {
    (void)l;

    struct wlr_xdg_activation_v1_request_activate_event *ev = data;
    if (ev == NULL || ev->token == NULL) return;

    struct ActivationContext *ctx = ev->token->data;
    struct Window *window = window_from_surface(ev->surface);

    bool allowed = false;
    if (ctx != NULL) {
        if (ctx->is_compositor) {
            allowed = true;
        } else if (ctx->seat_client != NULL) {
            allowed = wlr_seat_client_validate_event_serial(
                ctx->seat_client, ev->token->serial
            );

            if (!allowed) {
                log_debug(
                    "Rejected serial=%u for seat_client=%p", ev->token->serial,
                    (void *)ctx->seat_client
                );
            }
        } else {
            log_debug(
                "No seat client for token (surface=%p)",
                (void *)ev->token->surface
            );
        }
    }

    if (allowed && window != NULL && !window->closed) {
        if (!window->mapped) {
            if (CFG_FOCUS_ON_ACTIVATE && ctx != NULL) {
                wm_assign_new_window_space(window, ctx->space);
            }
        } else if (CFG_FOCUS_ON_ACTIVATE) {
            if (window->space != wm.current_space) {
                wm_switch_space(window->space);
            }
            seat_focus(window);
        } else {
            log_debug("Suppressed by config");
        }
    }

    ev->token->data = NULL;
    context_free(ctx);
}

static void handle_new_token(struct wl_listener *l, void *data) {
    (void)l;

    struct wlr_xdg_activation_token_v1 *token = data;
    if (token == NULL) return;

    struct ActivationContext *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) return;

    ctx->token = token;
    ctx->space = wm.current_space;
    ctx->is_compositor = false;

    ctx->origin_focus =
        server.seat ? server.seat->keyboard_state.focused_surface : NULL;

    if (ctx->origin_focus != NULL) {
        ctx->origin_focus_destroy.notify = handle_origin_focus_destroy;
        wl_signal_add(
            &ctx->origin_focus->events.destroy, &ctx->origin_focus_destroy
        );
    }

    ctx->seat_client = seat_client_for_token(token);

    token->data = ctx;

    ctx->token_destroy.notify = handle_token_destroy;
    wl_signal_add(&token->events.destroy, &ctx->token_destroy);
}

void activation_init(struct wlr_xdg_activation_v1 *activation) {
    if (activation == NULL || activation_initialized) return;

    new_token_listener.notify = handle_new_token;
    wl_signal_add(&activation->events.new_token, &new_token_listener);

    request_activate_listener.notify = handle_request_activate;
    wl_signal_add(
        &activation->events.request_activate, &request_activate_listener
    );

    activation_initialized = true;
}

void activation_finish(void) {
    if (!activation_initialized) return;

    wl_list_remove(&new_token_listener.link);
    wl_list_remove(&request_activate_listener.link);

    activation_initialized = false;
}

const char *activation_mint_spawn_token(void) {
    if (server.xdg_activation == NULL || server.seat == NULL) return NULL;

    struct wlr_xdg_activation_token_v1 *token =
        wlr_xdg_activation_token_v1_create(server.xdg_activation);
    if (token == NULL) return NULL;

    struct ActivationContext *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        wlr_xdg_activation_token_v1_destroy(token);
        return NULL;
    }

    ctx->token = token;
    ctx->space = wm.current_space;
    ctx->is_compositor = true;

    token->seat = server.seat;
    token->data = ctx;

    ctx->token_destroy.notify = handle_token_destroy;
    wl_signal_add(&token->events.destroy, &ctx->token_destroy);

    return wlr_xdg_activation_token_v1_get_name(token);
}
